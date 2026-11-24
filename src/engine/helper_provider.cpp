#include "helper_provider.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

#include "engine/model_speed.h"
#include "engine/paths.h"
#include "engine/settings.h"

namespace fugue {

namespace {

constexpr int kDefaultIdleSeconds = 600;    // spec 8.8's example start block
constexpr double kMaxIdleSeconds = 1000000; // about 11 days: more would overflow the timer
constexpr int kLogLines = 20;

const QString kEngine = QStringLiteral("sdcpp");

Error helperError(const QJsonObject &error) {
    const QString code = error.value("code").toString();
    const QString message = error.value("message").toString(QStringLiteral("The Fugue helper failed."));
    // Out of memory and a cancel keep their codes (8.8, 9.5); everything else the helper says
    // is about this job's work.
    if (code == QLatin1String("out_of_memory") || code == QLatin1String("cancelled"))
        return makeError(code, message);
    return makeError(QStringLiteral("node_failed"), message);
}

// The helper's log ends with the library's own line when a device allocation failed: stable-diffusion.cpp
// then asserts and aborts the process instead of returning an error (the second helper of one GPU, or
// another program, holds the memory; a CUDA `cudaMalloc failed: out of memory`, a Vulkan out-of-device-memory).
bool endsOutOfMemory(const QStringList &log) {
    for (const QString &line : log)
        if (line.contains(QLatin1String("out of memory"), Qt::CaseInsensitive) ||
            line.contains(QLatin1String("ErrorOutOfDeviceMemory")))
            return true;
    return false;
}

bool executable(const QString &path) {
    const QFileInfo info(path);
    return info.isFile() && info.isExecutable();
}

} // namespace

QStringList helperCapabilities() {
    return {QStringLiteral("text.encode"), QStringLiteral("latent.sample"), QStringLiteral("latent.decode"),
            QStringLiteral("latent.encode"), QStringLiteral("image.upscale"), QStringLiteral("image.restore")};
}

bool hasNvidiaGpu() { return QFileInfo::exists(QStringLiteral("/proc/driver/nvidia/version")); }

QString helperProgram(const QStringList &folders, bool nvidia) {
    QStringList names;
    if (nvidia)
        names << QStringLiteral("fugue-helper-cuda");
    names << QStringLiteral("fugue-helper-vulkan") << QStringLiteral("fugue-helper-cpu");
    for (const QString &folder : folders)
        for (const QString &name : std::as_const(names))
            if (const QString path = QDir(folder).filePath(name); executable(path))
                return path;
    return {};
}

QStringList helperFolders() {
    const QString app = QCoreApplication::applicationDirPath();
    QStringList folders{QDir(app).filePath(QStringLiteral("../lib/fugue"))};
    for (const char *backend : {"cuda", "vulkan", "cpu"})
        folders << QDir(app).filePath(QStringLiteral("../build-helper-%1").arg(QLatin1String(backend)));
    return folders;
}

QString helperProgram(bool nvidia) {
    if (qEnvironmentVariableIsSet("FUGUE_HELPER")) // empty: none, as the tests have it
        return qEnvironmentVariable("FUGUE_HELPER");
    return helperProgram(helperFolders(), nvidia);
}

QString CudaHelperNotice::command() { return QStringLiteral("sudo pacman -S fugue-helper-cuda"); }

namespace {
QString noticeMarker(const QString &stateFolder) {
    return QDir(stateFolder).filePath(QStringLiteral("cuda-helper-notice-dismissed"));
}
} // namespace

bool CudaHelperNotice::due(const QStringList &folders, bool nvidia, const QString &stateFolder) {
    if (!nvidia || QFileInfo::exists(noticeMarker(stateFolder)))
        return false;
    return QFileInfo(helperProgram(folders, true)).fileName() == QLatin1String("fugue-helper-vulkan");
}

void CudaHelperNotice::dismiss(const QString &stateFolder) {
    QDir().mkpath(stateFolder);
    QFile marker(noticeMarker(stateFolder));
    if (marker.open(QIODevice::WriteOnly))
        marker.close();
}

bool CudaHelperNotice::due() {
    return !qEnvironmentVariableIsSet("FUGUE_HELPER") && due(helperFolders(), hasNvidiaGpu(), stateDir());
}

void CudaHelperNotice::dismiss() { dismiss(stateDir()); }

HelperProvider::HelperProvider(const ProviderInstance &instance, QString program, int graceMs, QObject *parent)
    : HelperProvider(instance, StdioLaunch{std::move(program), {QStringLiteral("--serve")}, {}}, graceMs, parent) {}

HelperProvider::HelperProvider(const ProviderInstance &instance, StdioLaunch launch, int graceMs, QObject *parent)
    : Provider(parent), m_instance(instance), m_launch(std::move(launch)), m_program(m_launch.program), m_graceMs(graceMs) {
    // A number, or text as the settings page stores it.
    const QJsonValue setting = instance.settings.value("idle_timeout_s");
    bool ok = setting.isDouble();
    double idle = setting.toDouble();
    if (setting.isString())
        idle = setting.toString().toDouble(&ok);
    if (!ok)
        idle = kDefaultIdleSeconds;
    m_idleMs = idle > 0 ? int(std::min(idle, kMaxIdleSeconds) * 1000) : 0;
    m_idle.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, [this] { stop(); });
    m_yield.setInterval(500);
    connect(&m_yield, &QTimer::timeout, this, [this] {
        if (m_yieldProbe && m_yieldProbe())
            stop();
    });
}

HelperProvider::~HelperProvider() {
    if (!m_process)
        return;
    m_process->disconnect();
    m_process->kill();
    m_process->waitForFinished(3000);
}

qint64 HelperProvider::pid() const {
    return m_process && m_process->state() == QProcess::Running ? m_process->processId() : 0;
}

Error HelperProvider::unreachable(const QString &why) const {
    return makeError(QStringLiteral("provider_unreachable"),
                     why.isEmpty() ? QStringLiteral("%1 has no Fugue helper to run.").arg(m_instance.name) : why,
                     m_launch.hint);
}

QString HelperProvider::lastLog() const { return m_log.join(QLatin1Char('\n')); }

void HelperProvider::describe(Reply<Description> reply) {
    if (m_program.isEmpty())
        return later(std::move(reply), Result<Description>(unreachable()));
    if (m_launch.describeOverProtocol) {
        if (!m_process) {
            const QString why = start();
            if (!why.isEmpty())
                return later(std::move(reply), Result<Description>(unreachable(why)));
        }
        return send(QJsonObject{{"op", "describe"}}, [this, reply](const Result<QJsonObject> &answer) {
            if (!answer)
                return reply(Result<Description>(answer.errors()));
            // A stdio provider is a process on this machine: it shares the filesystem unless it says not.
            QJsonObject described = answer.value();
            if (!described.contains("shares_filesystem"))
                described.insert("shares_filesystem", true);
            reply(Result<Description>(parseDescription(described, m_launch.engine, {})));
        });
    }
    // Answered here, not by the helper: a model picker describes every provider each time it
    // opens, and that must not start a GPU process.
    Description description;
    description.engine = kEngine;
    for (const QString &capability : helperCapabilities()) {
        OfferedModel files;
        files.title = QStringLiteral("Model files on this machine");
        files.status = ModelStatus::Ready;
        description.capabilities.append(CapabilityOffer{capability, {files}});
    }
    later(std::move(reply), Result<Description>(description));
}

void HelperProvider::submit(const JobRequest &request, Reply<QString> reply) {
    if (m_program.isEmpty())
        return later(std::move(reply), Result<QString>(unreachable()));
    if (!m_process) {
        const QString why = start();
        if (!why.isEmpty()) // it failed at once (a file that is not a program)
            return later(std::move(reply), Result<QString>(unreachable(why)));
    }
    const int generation = m_generation;
    const std::optional<Job::Speed> speed = speedProbe(request);
    QJsonObject message{{"op", "submit"},
                        {"capability", request.capability},
                        {"model", request.model},
                        {"params", request.params},
                        {"inputs", request.inputs},
                        {"output_dir", request.outputDir}};
    // The picture of the sampling (8.10): a message key, not a param, so it is not in the cache key and
    // a helper that does not know it ignores it.
    const QString preview = previewMode(request.capability);
    if (!preview.isEmpty())
        message.insert("preview", preview);
    send(message,
         [this, reply, generation, speed](const Result<QJsonObject> &answer) {
             if (!answer)
                 return reply(Result<QString>(answer.error()));
             const QString helperJob = answer->value("job").toString();
             const QString id = QStringLiteral("%1.%2").arg(generation).arg(helperJob);
             Job job;
             job.status.state = QStringLiteral("queued");
             job.generation = generation;
             job.helperJob = helperJob;
             job.speed = speed;
             m_jobs.insert(id, job);
             m_byHelperJob.insert(helperJob, id);
             reply(Result<QString>(id));
             updateIdle();
         });
}

void HelperProvider::status(const QString &job, Reply<JobStatus> reply) {
    const auto found = m_jobs.constFind(job);
    if (found == m_jobs.constEnd())
        return later(std::move(reply),
                     Result<JobStatus>(makeError(QStringLiteral("unknown_run"), QStringLiteral("No job %1.").arg(job))));
    later(std::move(reply), Result<JobStatus>(found->status));
}

void HelperProvider::cancel(const QString &job, Reply<QString> reply) {
    const auto found = m_jobs.find(job);
    if (found == m_jobs.end())
        return later(std::move(reply),
                     Result<QString>(makeError(QStringLiteral("unknown_run"), QStringLiteral("No job %1.").arg(job))));
    if (found->status.isTerminal() || found->generation != m_generation || !m_process)
        return later(std::move(reply), Result<QString>(found->status.state));
    found->cancelled = true;
    send(QJsonObject{{"op", "cancel"}, {"job", found->helperJob}}, [this, job, reply](const Result<QJsonObject> &) {
        reply(Result<QString>(m_jobs.value(job).status.state));
    });
    // A job that does not stop within the grace period is ended with the helper (9.6).
    const int generation = m_generation;
    QTimer::singleShot(m_graceMs, this, [this, job, generation] {
        if (m_process && m_generation == generation && !m_jobs.value(job).status.isTerminal())
            m_process->kill();
    });
}

void HelperProvider::result(const QString &job, Reply<JobResult> reply) {
    const auto found = m_jobs.constFind(job);
    if (found == m_jobs.constEnd() || found->status.state != QLatin1String("done"))
        return later(std::move(reply), Result<JobResult>(makeError(QStringLiteral("unknown_run"),
                                                                   QStringLiteral("Job %1 has no result.").arg(job))));
    later(std::move(reply), Result<JobResult>(found->result));
}

void HelperProvider::shutdown(Reply<Nothing> reply) {
    // Returns once the helper is gone, so the memory is back when the caller measures again
    // (the scheduler makes room this way, 9.3).
    if (QProcess *process = stop(); process && !process->waitForFinished(m_graceMs)) {
        process->kill();
        process->waitForFinished(1000);
    }
    later(std::move(reply), Result<Nothing>(Nothing{}));
}

QString HelperProvider::start() {
    ++m_generation;
    m_out.clear();
    m_err.clear();
    m_log.clear();
    m_byHelperJob.clear();
    // The jobs of earlier helpers are over (abandon), their results taken.
    for (auto it = m_jobs.begin(); it != m_jobs.end();)
        it = it->status.isTerminal() ? m_jobs.erase(it) : std::next(it);
    m_process = new QProcess(this);
    QProcess *process = m_process;
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (process == m_process)
            readOutput(process);
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        if (process != m_process)
            return;
        m_err += process->readAllStandardError();
        for (const QByteArray &line : takeLines(m_err)) {
            const QString text = QString::fromUtf8(line).trimmed();
            if (!text.isEmpty())
                m_log.append(text.size() > 500 ? text.left(500) + QStringLiteral("…") : text);
        }
        while (m_log.size() > kLogLines)
            m_log.removeFirst();
    });
    connect(process, &QProcess::finished, this, [this, process] {
        if (process == m_process)
            finished();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process == m_process && error == QProcess::FailedToStart)
            finished();
    });
    if (!m_launch.workingDirectory.isEmpty())
        process->setWorkingDirectory(m_launch.workingDirectory);
    process->start(m_program, m_launch.arguments);
    if (!m_process) // finished() ran inside start()
        return m_startError;
    return {};
}

// The helper's preview mode for a job (settings.json's step_preview): "fast" is the helper's cheap
// "proj"; empty for no picture, or for a job that makes none.
QString HelperProvider::previewMode(const QString &capability) {
    if (capability != QLatin1String("latent.sample"))
        return {};
    const QString setting = currentSettings().stepPreview;
    return setting == QLatin1String("fast") ? QStringLiteral("proj") : setting == QLatin1String("off") ? QString() : setting;
}

std::optional<HelperProvider::Job::Speed> HelperProvider::speedProbe(const JobRequest &request) {
    if (request.capability != QLatin1String("latent.sample") || request.params.contains(QStringLiteral("video_frames")))
        return std::nullopt;
    // Only a sampling at guidance 1 (a distilled model: one pass a step) compares like with like.
    if (request.params.value(QStringLiteral("sample.guidance.txt_cfg")).toDouble(1) != 1.0)
        return std::nullopt;
    const QJsonObject context = request.params.value(QStringLiteral("context")).toObject();
    const QString file = context.contains(QStringLiteral("diffusion_model_path"))
                             ? context.value(QStringLiteral("diffusion_model_path")).toString()
                             : context.value(QStringLiteral("model_path")).toString();
    const double megapixels = request.params.value(QStringLiteral("width")).toDouble() *
                              request.params.value(QStringLiteral("height")).toDouble() / 1e6;
    if (file.isEmpty() || megapixels <= 0)
        return std::nullopt;
    Job::Speed speed;
    speed.modelFile = file;
    speed.megapixels = megapixels;
    return speed;
}

void HelperProvider::noteSpeed(Job &job, const QJsonObject &message) {
    if (!job.speed || m_launch.describeOverProtocol) // a module of its own is not the helper whose device this is
        return;
    Job::Speed &speed = *job.speed;
    const QString state = message.value("state").toString();
    const double progress = message.value("progress").toDouble();
    if (state == QLatin1String("running") && progress > 0 && progress < 1) {
        if (!m_clock.isValid())
            m_clock.start();
        const qint64 now = m_clock.elapsed();
        if (speed.firstProgress == 0) {
            speed.firstProgress = progress;
            speed.firstMs = now;
        }
        speed.lastProgress = progress;
        speed.lastMs = now;
    } else if (state == QLatin1String("done") && speed.firstProgress > 0) {
        // The first line seen is step 1, so its progress is 1 / steps; the last is the step before the end
        // or the end. At least three steps between them, else the figure is mostly noise.
        const double steps = std::round(1.0 / speed.firstProgress);
        const double between = std::round((speed.lastProgress - speed.firstProgress) * steps);
        if (steps >= 2 && steps <= 200 && between >= 3 && speed.lastMs > speed.firstMs)
            ModelSpeeds().record(localComputeDevice().id(), speed.modelFile,
                                 (speed.lastMs - speed.firstMs) / 1000.0 / between / speed.megapixels);
        job.speed.reset();
    }
}

QList<QByteArray> HelperProvider::takeLines(QByteArray &buffer) {
    QList<QByteArray> lines;
    qsizetype end;
    while ((end = buffer.indexOf('\n')) >= 0) {
        lines.append(buffer.left(end));
        buffer.remove(0, end + 1);
    }
    return lines;
}

void HelperProvider::send(QJsonObject message, Answer answer) {
    if (!m_process) // it stopped while a reply was being read
        return answer(Result<QJsonObject>(stopped(m_launch.label + QStringLiteral(" stopped."))));
    const QString id = QString::number(++m_nextId);
    message.insert("id", id);
    m_waiting.insert(id, std::move(answer));
    m_idle.stop();
    m_yield.stop();
    m_process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

Error HelperProvider::stopped(const QString &why) { return makeError(QStringLiteral("node_failed"), why); }

void HelperProvider::readOutput(QProcess *process) {
    m_out += process->readAllStandardOutput();
    for (const QByteArray &text : takeLines(m_out)) {
        const QJsonDocument document = QJsonDocument::fromJson(text);
        if (document.isObject())
            line(document.object());
        // Anything else on stdout is not the protocol's; the log goes to stderr.
    }
}

void HelperProvider::line(const QJsonObject &message) {
    const QString id = message.value("id").toString();
    if (!id.isEmpty() && m_waiting.contains(id)) {
        const Answer answer = m_waiting.take(id);
        answer(message.contains("error") ? Result<QJsonObject>(helperError(message.value("error").toObject()))
                                         : Result<QJsonObject>(message));
        updateIdle();
        return;
    }
    if (message.value("op").toString() == QLatin1String("status"))
        helperStatus(message.value("job").toString(), message);
}

void HelperProvider::helperStatus(const QString &helperJob, const QJsonObject &message) {
    const QString id = m_byHelperJob.value(helperJob);
    if (id.isEmpty() || m_jobs.value(id).status.isTerminal())
        return;
    JobStatus status;
    status.state = message.value("state").toString();
    status.progress = message.value("progress").toDouble();
    status.stage = message.value("stage").toString();
    noteSpeed(m_jobs[id], message);
    // A frame of the sampling: a picture file the helper just wrote, read at once (it writes the next over it).
    if (const QString file = message.value("preview").toString(); !file.isEmpty()) {
        QFile frame(file);
        if (frame.open(QIODevice::ReadOnly))
            status.preview = frame.readAll();
    }
    if (status.state == QLatin1String("error"))
        status.error = helperError(message.value("error").toObject());
    if (status.state != QLatin1String("done"))
        return change(id, status);
    // Fetched at once, so a result() later needs no helper (it may have stopped by then).
    send(QJsonObject{{"op", "result"}, {"job", helperJob}}, [this, id, status](const Result<QJsonObject> &answer) {
        JobStatus over = status;
        if (!answer) {
            over.state = QStringLiteral("error");
            over.error = answer.error();
        } else {
            m_jobs[id].result = JobResult{answer->value("outputs").toObject(), answer->value("used").toObject()};
        }
        change(id, over);
    });
}

void HelperProvider::change(const QString &job, const JobStatus &status) {
    m_jobs[job].status = status;
    if (status.isTerminal())
        m_byHelperJob.remove(m_jobs.value(job).helperJob);
    emit statusChanged(job, status);
    updateIdle();
}

void HelperProvider::finished() {
    QProcess *process = std::exchange(m_process, nullptr);
    const bool startFailed = process->error() == QProcess::FailedToStart;
    if (!startFailed)
        readOutput(process);
    process->deleteLater();
    m_startError = startFailed ? QStringLiteral("%1 could not be started: %2.").arg(m_launch.label, process->errorString())
                               : QString();
    QString why = startFailed ? m_startError
                              : m_launch.label + QStringLiteral(" stopped while running this node; it starts again on the next run.");
    if (!m_log.isEmpty())
        why += QStringLiteral(" Its log ends:\n") + lastLog();
    // A helper that died of a failed allocation is out of memory, as one that said so (8.10): the scheduler
    // frees a resident model and tries again, and names who holds the memory when nothing is left to free.
    abandon(startFailed ? unreachable(why)
            : endsOutOfMemory(m_log) ? makeError(QStringLiteral("out_of_memory"), why)
                                     : stopped(why));
}

void HelperProvider::abandon(const Error &why) {
    m_idle.stop();
    m_yield.stop();
    const QHash<QString, Answer> waiting = std::exchange(m_waiting, {});
    for (const Answer &answer : waiting)
        answer(Result<QJsonObject>(why));
    // A provider process that dies fails the jobs it held (8.8).
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
        if (it->status.isTerminal() || it->generation != m_generation)
            continue;
        JobStatus status;
        if (it->cancelled) {
            status.state = QStringLiteral("cancelled");
        } else {
            status.state = QStringLiteral("error");
            status.error = why.code == QLatin1String("out_of_memory") ? why : stopped(why.message);
        }
        it->status = status;
        emit statusChanged(it.key(), status);
    }
    m_byHelperJob.clear();
}

QProcess *HelperProvider::stop() {
    if (!m_process)
        return nullptr;
    // Let go of it now, so a job submitted meanwhile starts a new one; it is asked to stop, and
    // killed when it is not out within the grace period (8.10: the memory goes back either way).
    QProcess *process = std::exchange(m_process, nullptr);
    process->disconnect(this);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        process = nullptr;
    } else {
        connect(process, &QProcess::finished, process, &QObject::deleteLater);
        process->write(QJsonDocument(QJsonObject{{"op", "shutdown"}, {"id", QString::number(++m_nextId)}})
                           .toJson(QJsonDocument::Compact) +
                       '\n');
        process->closeWriteChannel();
        QTimer::singleShot(m_graceMs, process, [process] { process->kill(); });
    }
    abandon(stopped(m_launch.label + QStringLiteral(" was stopped while running this node.")));
    return process;
}

void HelperProvider::setYieldProbe(std::function<bool()> probe) {
    m_yieldProbe = std::move(probe);
    updateIdle();
}

void HelperProvider::updateIdle() {
    const auto busy = [this] {
        if (!m_process || !m_waiting.isEmpty())
            return true;
        for (const Job &job : std::as_const(m_jobs))
            if (job.generation == m_generation && !job.status.isTerminal())
                return true;
        return false;
    };
    if (busy()) {
        m_idle.stop();
        m_yield.stop();
        return;
    }
    // No job: a run of another root that waits for the GPU gets the memory now, not at the idle timeout.
    if (m_yieldProbe) {
        if (m_yieldProbe()) {
            m_yield.stop();
            stop();
            return;
        }
        m_yield.start();
    }
    if (m_idleMs <= 0)
        m_idle.stop();
    else
        m_idle.start(m_idleMs);
}

} // namespace fugue
