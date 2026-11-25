#include "remote_helper_provider.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QUrl>

#include "engine/helper_provider.h"

namespace fugue {

namespace {

constexpr int kDefaultPollMs = 500;
constexpr int kTransferTimeoutMs = 120000; // of silence: a large upload or download is not one
// describe on a paired helper is a few bytes: a host that does not answer it in this long is gone (#395), so a run or a check says so
// at once instead of waiting out the transfer timeout. Other http providers (modules, which start_timeout_s governs) keep theirs.
constexpr int kDescribeTimeoutMs = 10000;
constexpr int kLostPolls = 3;              // status requests in a row that may fail before the job does

Error hostError(const QJsonObject &error) {
    const QString code = error.value("code").toString();
    const QString message = error.value("message").toString(QStringLiteral("The provider failed."));
    // MOSS Image's tokenizer is a file the host's engine reads (#544): without it the engine says to pass one on its command line,
    // which is not something a person here can do. The host's tokenizers folder is where it goes.
    if (message.contains(QLatin1String("--tokenizer")) || message.contains(QLatin1String("qwen3.5_tokenizer")) ||
        (message.contains(QLatin1String("requires")) && message.contains(QLatin1String("tokenizer"), Qt::CaseInsensitive)))
        return makeError(QStringLiteral("model_missing"),
                         QStringLiteral("The paired host has no Qwen3.5 tokenizer (qwen3.5_tokenizer.json), which MOSS Image reads."),
                         QStringLiteral("Put qwen3.5_tokenizer.json in the tokenizers folder of the paired host (Get ready here downloads it to this machine only)."));
    // These keep their codes (8.5, 8.8, 9.5); everything else the host says is about this job's work.
    if (code == QLatin1String("out_of_memory") || code == QLatin1String("cancelled") ||
        code == QLatin1String("model_missing"))
        return makeError(code, message);
    return makeError(QStringLiteral("node_failed"), message);
}

// How a file goes to the host: a stage file by the input it is (their names are the host's to
// read back), an image by its extension.
QString formatOf(const QString &input, const QString &path) {
    if (input == QLatin1String("conditioning"))
        return input;
    if (input == QLatin1String("latent") || input == QLatin1String("init_latent"))
        return QStringLiteral("latent");
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("jpeg") || suffix == QLatin1String("jpg"))
        return QStringLiteral("jpg");
    return suffix.isEmpty() ? QStringLiteral("png") : suffix;
}

// The file as {"data", "format"}, or why it cannot be read.
Result<QJsonObject> asData(const QString &input, const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return makeError(QStringLiteral("node_failed"),
                         QStringLiteral("Cannot read %1 to send it to the paired host: %2.").arg(path, file.errorString()));
    return QJsonObject{{"data", QString::fromLatin1(file.readAll().toBase64())}, {"format", formatOf(input, path)}};
}

// `value` with every {"path": p} replaced by the file's data; a list item by item.
Result<QJsonValue> dataOf(const QString &input, const QJsonValue &value) {
    if (value.isArray()) {
        QJsonArray items;
        for (const QJsonValue &item : value.toArray()) {
            const Result<QJsonValue> one = dataOf(input, item);
            if (!one)
                return one.errors();
            items.append(one.value());
        }
        return QJsonValue(items);
    }
    const QJsonObject object = value.toObject();
    if (!object.contains(QStringLiteral("path")))
        return value;
    const Result<QJsonObject> data = asData(input, object.value(QStringLiteral("path")).toString());
    if (!data)
        return data.errors();
    return QJsonValue(data.value());
}

// What the host names is data: a file name is only letters, digits, - and _, a format is a short
// word, so no name or format makes a path outside the job's folder.
QString safeName(const QString &name) {
    QString out;
    for (const QChar c : name)
        out += (c.isLetterOrNumber() && c.unicode() < 128) || c == QLatin1Char('-') || c == QLatin1Char('_') ? c : QLatin1Char('_');
    return out.isEmpty() ? QStringLiteral("out") : out.left(64);
}

QString extensionOf(const QString &format) {
    static const QRegularExpression word(QStringLiteral("^[a-z0-9]{1,12}$"));
    return word.match(format).hasMatch() ? format : QStringLiteral("bin");
}

constexpr qint64 kMaxDownloadBytes = qint64(4) << 30; // the largest file taken from a host

} // namespace

RemoteHelperProvider::RemoteHelperProvider(const ProviderInstance &instance, int pollMs, QObject *parent)
    : RemoteHelperProvider(instance, [] {
          RemoteProviderOptions options; // the paired helper's
          options.defaultCapabilities = helperCapabilities();
          return options;
      }(), pollMs, parent) {}

RemoteHelperProvider::RemoteHelperProvider(const ProviderInstance &instance, RemoteProviderOptions options, int pollMs,
                                           QObject *parent)
    : Provider(parent), m_instance(instance), m_options(std::move(options)) {
    m_network.setTransferTimeout(kTransferTimeoutMs);
    m_poll.setInterval(pollMs > 0 ? pollMs : kDefaultPollMs);
    connect(&m_poll, &QTimer::timeout, this, &RemoteHelperProvider::poll);
    m_idle.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, [this] {
        for (const Job &job : std::as_const(m_jobs))
            if (!job.status.isTerminal())
                return touch(); // a job is still out: wait again
        stopServer();
    });
    m_startLimit.setSingleShot(true);
    connect(&m_startLimit, &QTimer::timeout, this, [this] {
        startDone(QStringLiteral("it did not answer describe in %1 seconds.").arg(m_options.startTimeoutMs / 1000));
    });
}

RemoteHelperProvider::~RemoteHelperProvider() {
    if (!m_process)
        return;
    m_process->disconnect();
    m_process->kill();
    m_process->waitForFinished(3000);
}

QString RemoteHelperProvider::address() const { return launched() ? m_runAddress : m_instance.address; }

Error RemoteHelperProvider::unreachable(const QString &why) const {
    return makeError(QStringLiteral("provider_unreachable"),
                     QStringLiteral("%1 (%2): %3").arg(m_instance.name, address(), why),
                     launched() ? QStringLiteral("Check the provider's own log; Fugue starts it when it is needed.")
                                : QStringLiteral("Check that %1 is running and that its address and key in providers.json "
                                                 "are right.")
                                      .arg(m_options.where == QLatin1String("the paired host") ? QStringLiteral("the Fugue helper on the host")
                                                                                              : QStringLiteral("the provider")));
}

QNetworkReply *RemoteHelperProvider::request(const QString &method, const QString &path, const QByteArray &body) {
    QNetworkRequest request(QUrl(address() + path));
    if (path.endsWith(QLatin1String("/describe")) && isPairedHelper(m_instance))
        request.setTransferTimeout(kDescribeTimeoutMs);
    if (!m_instance.key.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_instance.key.toUtf8());
    // The host answers where it is asked: a redirect would carry the key elsewhere.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!body.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    return m_network.sendCustomRequest(request, method.toLatin1(), body);
}

Result<QJsonObject> RemoteHelperProvider::parse(QNetworkReply *reply) const {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    if (status == 401 || status == 403)
        return unreachable(QStringLiteral("the host refused the key (HTTP %1).").arg(status));
    if (object.value("error").isObject())
        return hostError(object.value("error").toObject());
    if (status == 0)
        return unreachable(QStringLiteral("cannot reach it: %1.").arg(reply->errorString()));
    if (status >= 400 || reply->error() != QNetworkReply::NoError)
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 answered HTTP %2.").arg(m_options.where, QString::number(status)));
    return object;
}

void RemoteHelperProvider::sendPost(const QString &op, const QJsonObject &body, Answer answer) {
    QNetworkReply *reply = request(QStringLiteral("POST"), QStringLiteral("/fugue/v1/") + op,
                                   QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, answer = std::move(answer)] {
        const Result<QJsonObject> parsed = parse(reply);
        reply->deleteLater();
        answer(parsed);
    });
}

void RemoteHelperProvider::describe(Reply<Description> reply) {
    post(QStringLiteral("describe"), {}, [this, reply](const Result<QJsonObject> &answer) {
        if (!answer)
            return reply(Result<Description>(answer.errors()));
        QJsonObject described = answer.value();
        if (!described.contains("capabilities") && !m_options.defaultCapabilities.isEmpty())
            described.insert("capabilities", QJsonArray::fromStringList(m_options.defaultCapabilities));
        reply(Result<Description>(parseDescription(described, m_options.defaultEngine, m_options.filesTitle)));
    }, true);
}

void RemoteHelperProvider::submit(const JobRequest &request, Reply<QString> reply) {
    // The files go with the job, read now: a stage file of an earlier node is already in the run folder.
    QJsonObject inputs;
    for (auto it = request.inputs.begin(); it != request.inputs.end(); ++it) {
        const Result<QJsonValue> data = dataOf(it.key(), it.value());
        if (!data)
            return later(std::move(reply), Result<QString>(data.errors()));
        inputs.insert(it.key(), data.value());
    }
    // Edit references are files in the generation options (ref_images: [{"image": PATH}]).
    QJsonObject params = request.params;
    if (params.value("ref_images").isArray()) {
        QJsonArray references;
        for (const QJsonValue &item : params.value("ref_images").toArray()) {
            QJsonObject reference = item.toObject();
            if (reference.value("image").isString()) {
                const Result<QJsonObject> data = asData(QStringLiteral("ref_images"), reference.value("image").toString());
                if (!data)
                    return later(std::move(reply), Result<QString>(data.errors()));
                reference.insert("image", data.value());
            }
            references.append(reference);
        }
        params.insert("ref_images", references);
    }
    const QString outputDir = request.outputDir;
    QJsonObject message{{"capability", request.capability}, {"params", params}, {"inputs", inputs}};
    if (!request.model.isEmpty())
        message.insert("model", request.model);
    post(QStringLiteral("submit"), message,
         [this, reply, outputDir](const Result<QJsonObject> &answer) {
             if (!answer)
                 return reply(Result<QString>(answer.errors()));
             const QString hostJob = answer->value("job").toString();
             Job job;
             job.hostJob = hostJob;
             job.outputDir = outputDir;
             job.status.state = QStringLiteral("queued");
             m_jobs.insert(hostJob, job);
             if (!m_poll.isActive())
                 m_poll.start();
             reply(Result<QString>(hostJob));
         }, true);
}

void RemoteHelperProvider::status(const QString &job, Reply<JobStatus> reply) {
    const auto found = m_jobs.constFind(job);
    if (found == m_jobs.constEnd())
        return later(std::move(reply),
                     Result<JobStatus>(makeError(QStringLiteral("unknown_run"), QStringLiteral("No job %1.").arg(job))));
    later(std::move(reply), Result<JobStatus>(found->status));
}

void RemoteHelperProvider::cancel(const QString &job, Reply<QString> reply) {
    const auto found = m_jobs.constFind(job);
    if (found == m_jobs.constEnd())
        return later(std::move(reply),
                     Result<QString>(makeError(QStringLiteral("unknown_run"), QStringLiteral("No job %1.").arg(job))));
    if (found->status.isTerminal())
        return later(std::move(reply), Result<QString>(found->status.state));
    post(QStringLiteral("cancel"), QJsonObject{{"job", job}}, [this, job, reply](const Result<QJsonObject> &) {
        reply(Result<QString>(m_jobs.value(job).status.state));
    });
}

void RemoteHelperProvider::result(const QString &job, Reply<JobResult> reply) {
    const auto found = m_jobs.constFind(job);
    if (found == m_jobs.constEnd() || found->status.state != QLatin1String("done"))
        return later(std::move(reply), Result<JobResult>(makeError(QStringLiteral("unknown_run"),
                                                                   QStringLiteral("Job %1 has no result.").arg(job))));
    later(std::move(reply), Result<JobResult>(found->result));
}

void RemoteHelperProvider::poll() {
    bool active = false;
    const QStringList ids = m_jobs.keys();
    for (const QString &id : ids) {
        Job &job = m_jobs[id];
        if (job.status.isTerminal())
            continue;
        active = true;
        if (job.polling)
            continue;
        job.polling = true;
        post(QStringLiteral("status"), QJsonObject{{"job", id}}, [this, id](const Result<QJsonObject> &answer) {
            if (!m_jobs.contains(id) || m_jobs.value(id).status.isTerminal())
                return;
            Job &job = m_jobs[id];
            if (!answer) {
                job.polling = false;
                // A host that cannot be reached for a few polls in a row has lost the job; an answer
                // that is an error is the job's (it ended).
                if (answer.error().code == QLatin1String("provider_unreachable") && ++job.failures < kLostPolls)
                    return;
                return fail(id, answer.error());
            }
            job.failures = 0;
            JobStatus status;
            status.state = answer->value("state").toString();
            status.progress = answer->value("progress").toDouble();
            status.stage = answer->value("stage").toString();
            if (status.state == QLatin1String("error"))
                status.error = hostError(answer->value("error").toObject());
            if (status.state == QLatin1String("done"))
                return fetch(id); // polling stays set until the files are here
            job.polling = false;
            if (status.state != job.status.state || status.progress != job.status.progress || status.stage != job.status.stage)
                change(id, status);
        });
    }
    if (!active)
        m_poll.stop();
}

void RemoteHelperProvider::fail(const QString &job, const Error &error) {
    JobStatus status;
    status.state = QStringLiteral("error");
    status.error = error;
    change(job, status);
}

void RemoteHelperProvider::change(const QString &job, const JobStatus &status) {
    Job &held = m_jobs[job];
    held.status = status;
    held.polling = false;
    if (status.isTerminal())
        touch();
    emit statusChanged(job, status);
}

namespace {

// What the result's files are to be fetched as: each {"file": i, "format", "size"} in `outputs`.
struct Download {
    QString name;  // the output, with -k for the k-th of a list
    int file = 0;
    QString format;
    qint64 size = -1;
    QStringList where; // the path in outputs: {name} or {name, index}
};

} // namespace

void RemoteHelperProvider::fetch(const QString &id) {
    post(QStringLiteral("result"), QJsonObject{{"job", id}}, [this, id](const Result<QJsonObject> &answer) {
        if (!answer)
            return fail(id, answer.error());
        const QString outputDir = m_jobs.value(id).outputDir;
        const QJsonObject outputs = answer->value("outputs").toObject();
        // The files, in order, then each is downloaded one after the other.
        struct Pending {
            QJsonObject outputs;
            QJsonObject used;
            QList<Download> files;
            QJsonObject placed; // outputs with the paths
            int next = 0;
        };
        auto pending = std::make_shared<Pending>();
        pending->outputs = outputs;
        pending->used = answer->value("used").toObject();
        pending->placed = outputs;
        const auto isFile = [](const QJsonValue &value) { return value.isObject() && value.toObject().contains("file"); };
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            if (isFile(it.value())) {
                const QJsonObject file = it.value().toObject();
                pending->files.append({it.key(), file.value("file").toInt(), file.value("format").toString(),
                                       qint64(file.value("size").toDouble(-1)), {it.key()}});
            } else if (it.value().isArray()) {
                const QJsonArray list = it.value().toArray();
                for (qsizetype k = 0; k < list.size(); ++k)
                    if (isFile(list.at(k))) {
                        const QJsonObject file = list.at(k).toObject();
                        pending->files.append({QStringLiteral("%1-%2").arg(it.key()).arg(k + 1), file.value("file").toInt(),
                                               file.value("format").toString(), qint64(file.value("size").toDouble(-1)),
                                               {it.key(), QString::number(k)}});
                    }
            }
        }
        auto step = std::make_shared<std::function<void()>>();
        QPointer<RemoteHelperProvider> self(this);
        std::weak_ptr<std::function<void()>> again = step; // not owned by itself
        *step = [self, id, outputDir, pending, again] {
            if (!self)
                return;
            if (pending->next >= pending->files.size()) {
                if (!self->m_jobs.contains(id))
                    return;
                self->m_jobs[id].result = JobResult{pending->placed, pending->used};
                JobStatus done;
                done.state = QStringLiteral("done");
                done.progress = 1;
                return self->change(id, done);
            }
            const Download &download = pending->files.at(pending->next);
            if (download.size < 0 || download.size > kMaxDownloadBytes)
                return self->fail(id, makeError(QStringLiteral("node_failed"),
                                                QStringLiteral("%2 gave no usable size for %1.").arg(download.name, self->m_options.where)));
            QNetworkReply *reply = self->request(QStringLiteral("GET"),
                                                 QStringLiteral("/fugue/v1/result/%1/%2").arg(QString::fromLatin1(QUrl::toPercentEncoding(id))).arg(download.file), {});
            const std::shared_ptr<std::function<void()>> next = again.lock();
            // A host that sends more than it said is cut off, so a body is never taken whole into memory.
            connect(reply, &QNetworkReply::downloadProgress, reply, [reply, limit = download.size](qint64 received, qint64) {
                if (received > limit)
                    reply->abort();
            });
            connect(reply, &QNetworkReply::finished, self.data(), [self, id, outputDir, pending, next, reply, download] {
                if (!self)
                    return;
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray bytes = reply->readAll();
                const QString failed = status == 401 || status == 403 ? QStringLiteral("the key was refused")
                                       : status == 0                  ? reply->errorString()
                                       : status >= 400                ? QStringLiteral("HTTP %1").arg(status)
                                       : download.size >= 0 && bytes.size() != download.size
                                           ? QStringLiteral("the file came short (%1 of %2 bytes)").arg(bytes.size()).arg(download.size)
                                           : QString();
                reply->deleteLater();
                if (!failed.isEmpty())
                    return self->fail(id, status == 0 || status == 401 || status == 403
                                              ? self->unreachable(QStringLiteral("cannot fetch %1: %2.").arg(download.name, failed))
                                              : makeError(QStringLiteral("node_failed"),
                                                          QStringLiteral("Cannot fetch %1 from %2: %3.").arg(download.name, self->m_options.where, failed)));
                const QString path = QDir(outputDir).filePath(QStringLiteral("%1-%2.%3").arg(safeName(download.name)).arg(download.file).arg(extensionOf(download.format)));
                QFile file(path);
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
                    return self->fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("Cannot write %1.").arg(path)));
                file.close();
                const QJsonObject placed{{"path", path}, {"format", download.format}};
                if (download.where.size() == 1) {
                    pending->placed.insert(download.where.first(), placed);
                } else {
                    QJsonArray list = pending->placed.value(download.where.first()).toArray();
                    list.replace(download.where.last().toInt(), placed);
                    pending->placed.insert(download.where.first(), list);
                }
                ++pending->next;
                (*next)();
            });
        };
        (*step)();
    });
}

void RemoteHelperProvider::post(const QString &op, const QJsonObject &body, Answer answer, bool mayStart) {
    if (!launched())
        return sendPost(op, body, std::move(answer));
    if (!mayStart && !m_up)
        return later(Reply<QJsonObject>(answer),
                     Result<QJsonObject>(unreachable(QStringLiteral("it is not running (it stopped, and starts again on the next job)."))));
    if (mayStart)
        touch();
    if (m_up)
        return sendPost(op, body, std::move(answer));
    whenUp([this, op, body, answer = std::move(answer)](const QString &why) {
        if (!why.isEmpty())
            return answer(Result<QJsonObject>(unreachable(why)));
        sendPost(op, body, answer);
    });
}

void RemoteHelperProvider::whenUp(Ready ready) {
    m_waiters.append(std::move(ready));
    if (m_starting)
        return;
    m_starting = true;
    // A free loopback port: bind one, read it, let go. Another process could take it in between;
    // the module then fails to start, and the next request tries a new one.
    QTcpServer finder;
    if (!finder.listen(QHostAddress::LocalHost, 0))
        return startDone(QStringLiteral("no free port on this machine: %1.").arg(finder.errorString()));
    const QString port = QString::number(finder.serverPort());
    finder.close();
    m_runAddress = QString(m_options.addressTemplate).replace(QStringLiteral("{port}"), port);
    QStringList arguments;
    for (const QString &argument : std::as_const(m_options.arguments))
        arguments.append(QString(argument).replace(QStringLiteral("{port}"), port));
    m_log.clear();
    m_process = new QProcess(this);
    QProcess *process = m_process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    if (!m_options.workingDirectory.isEmpty())
        process->setWorkingDirectory(m_options.workingDirectory);
    connect(process, &QProcess::readyRead, this, [this, process] {
        for (const QByteArray &line : process->readAll().split('\n')) {
            const QString text = QString::fromUtf8(line).trimmed();
            if (!text.isEmpty() && process == m_process)
                m_log.append(text.left(500));
        }
        while (m_log.size() > 20)
            m_log.removeFirst();
    });
    connect(process, &QProcess::finished, this, [this, process] {
        if (process != m_process)
            return;
        m_process = nullptr;
        m_up = false;
        process->deleteLater();
        if (m_starting)
            startDone(QStringLiteral("it exited before it answered."));
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process == m_process && error == QProcess::FailedToStart) {
            const QString why = QStringLiteral("it could not be started: %1.").arg(process->errorString());
            m_process = nullptr;
            process->deleteLater();
            startDone(why);
        }
    });
    process->start(m_options.program, arguments);
    m_startLimit.start(m_options.startTimeoutMs);
    QTimer::singleShot(0, this, &RemoteHelperProvider::probe);
}

void RemoteHelperProvider::probe() {
    if (!m_starting || !m_process)
        return;
    QProcess *process = m_process;
    sendPost(QStringLiteral("describe"), {}, [this, process](const Result<QJsonObject> &answer) {
        if (!m_starting || process != m_process)
            return;
        // Anything the server says, even an error, means it is listening.
        if (answer || answer.error().code != QLatin1String("provider_unreachable"))
            return startDone({});
        QTimer::singleShot(100, this, &RemoteHelperProvider::probe);
    });
}

void RemoteHelperProvider::startDone(const QString &why) {
    m_starting = false;
    m_startLimit.stop();
    m_up = why.isEmpty();
    QString reason = why;
    if (!why.isEmpty()) {
        if (!m_log.isEmpty())
            reason += QStringLiteral(" Its log ends:\n") + m_log.join(QLatin1Char('\n'));
        stopServer();
    }
    const QList<Ready> waiters = std::exchange(m_waiters, {});
    for (const Ready &ready : waiters)
        ready(reason);
}

void RemoteHelperProvider::touch() {
    if (launched() && m_options.idleMs > 0 && m_process)
        m_idle.start(m_options.idleMs);
}

QProcess *RemoteHelperProvider::stopServer() {
    m_idle.stop();
    if (!m_process)
        return nullptr;
    // Let go of it now, so a job submitted meanwhile starts a new one.
    QProcess *process = std::exchange(m_process, nullptr);
    const bool wasUp = std::exchange(m_up, false);
    const QString where = m_runAddress;
    process->disconnect(this);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return nullptr;
    }
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    if (wasUp) { // asked politely first (8.8); a server that does not know the operation is ended below
        QNetworkRequest shutdown(QUrl(where + QStringLiteral("/fugue/v1/shutdown")));
        if (!m_instance.key.isEmpty())
            shutdown.setRawHeader("Authorization", "Bearer " + m_instance.key.toUtf8());
        shutdown.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
        QNetworkReply *sent = m_network.post(shutdown, QByteArrayLiteral("{}"));
        connect(sent, &QNetworkReply::finished, sent, &QObject::deleteLater);
    }
    QTimer::singleShot(1000, process, [process] { process->terminate(); });
    QTimer::singleShot(5000, process, [process] { process->kill(); });
    return process;
}

void RemoteHelperProvider::shutdown(Reply<Nothing> reply) {
    // Returns once the process is gone, so its memory is back when the caller measures again (9.3).
    // The shutdown request goes out and the process is ended after a second (stopServer), both
    // from the event loop, so it is spun here until the process is over.
    if (QProcess *process = stopServer(); process && process->state() != QProcess::NotRunning) {
        QEventLoop wait;
        connect(process, &QProcess::finished, &wait, &QEventLoop::quit);
        QTimer::singleShot(7000, &wait, &QEventLoop::quit);
        wait.exec();
    }
    later(std::move(reply), Result<Nothing>(Nothing{}));
}

} // namespace fugue
