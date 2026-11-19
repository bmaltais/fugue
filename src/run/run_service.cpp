#include "run/run_service.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <memory>
#include <utility>

#include "cli/process.h"
#include "cli/runs.h"
#include "engine/compiler.h"
#include "engine/file_hashes.h"
#include "engine/paths.h"
#include "engine/queue.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"
#include "engine/scheduler.h"

namespace fugue {

namespace {

constexpr int kThumbnailWidth = 512;

QDateTime now() { return QDateTime::currentDateTimeUtc(); }

// A copy of the image at `path`, at most kThumbnailWidth wide, made once per content.
QJsonObject thumbnailOf(const QJsonObject &handle, const QString &dir) {
    const QString path = handle.value("path").toString();
    if (path.isEmpty())
        return {};
    QString key = handle.value("hash").toString().section(':', -1);
    if (key.isEmpty() || key.contains('/')) {
        const QFileInfo info(path);
        key = QString::fromLatin1(
            QCryptographicHash::hash((path + QString::number(info.lastModified().toMSecsSinceEpoch()) + ':'
                                      + QString::number(info.size()))
                                         .toUtf8(),
                                     QCryptographicHash::Sha256)
                .toHex());
    }
    const QString file = dir + '/' + key + QStringLiteral("-%1.png").arg(kThumbnailWidth);
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    int width = handle.value("width").toInt(size.width());
    int height = handle.value("height").toInt(size.height());
    if (width <= 0 || height <= 0)
        return {};
    if (QFile existing(file); existing.exists()) {
        // In use again: kept from pruning for another 7 days.
        if (existing.open(QIODevice::ReadWrite))
            existing.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    } else {
        if (size.isValid() && size.width() > kThumbnailWidth)
            reader.setScaledSize(QSize(kThumbnailWidth, qMax(1, size.height() * kThumbnailWidth / size.width())));
        const QImage image = reader.read();
        if (image.isNull() || !QDir().mkpath(dir))
            return {};
        // Written under another name and renamed, so a reader never sees half a file.
        const QString part = file + QStringLiteral(".part");
        if (!image.save(part, "PNG") || !QFile::rename(part, file)) {
            QFile::remove(part);
            if (!QFileInfo::exists(file))
                return {};
        }
    }
    // "source" is the full image, the one Open, Copy and Save Image act on.
    return {{"url", QUrl::fromLocalFile(file).toString()}, {"width", width}, {"height", height}, {"source", path}};
}

// What stands in for a thumbnail of a video or audio result: a label, and a video's shape.
// (A video's first frame would need ffmpeg per result; the label is what 4.5 falls back to.)
QJsonObject mediaLabel(const QJsonObject &handle) {
    const QString type = handle.value("type").toString();
    if (type != QLatin1String("video") && type != QLatin1String("audio"))
        return {};
    const int seconds = qRound(handle.value("duration").toDouble());
    const QString length = QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    QJsonObject label{{"label", (type == QLatin1String("video") ? RunService::tr("Video") : RunService::tr("Audio"))
                                    + QStringLiteral(" \u00b7 ") + length}};
    if (type == QLatin1String("video")) {
        label.insert("width", handle.value("width").toInt());
        label.insert("height", handle.value("height").toInt());
    }
    return label;
}

bool isImage(const QJsonValue &value) {
    return value.isObject() && value.toObject().value("type") == QLatin1String("image")
           && value.toObject().contains("path");
}

} // namespace

// Lives on the run thread; every method runs there.
class RunThread : public QObject {
    Q_OBJECT

public:
    explicit RunThread(RunSetup setup) : m_setup(std::move(setup)) {}

    ~RunThread() override = default;

    void init() {
        if (m_setup.addNodeTypes) {
            m_setup.addNodeTypes(m_context.registry);
            m_setup.addNodeTypes(m_compileRegistry);
        }
        if (m_setup.addKernels)
            m_setup.addKernels(m_context.kernels);
        if (!m_setup.registerProviders)
            m_setup.registerProviders = cli::registerProviders;
        m_context.runsRoot = m_setup.runsRoot.isEmpty() ? RunFolder::defaultRoot() : m_setup.runsRoot;
        m_context.queueDir = m_setup.queueDir.isEmpty() ? Queue::defaultDir() : m_setup.queueDir;
        m_context.lockPath = m_setup.lockPath.isEmpty() ? Queue::defaultLockPath() : m_setup.lockPath;
        m_process = std::make_unique<cli::Process>(
            m_context, m_setup.cacheRoot.isEmpty() ? ResultCache::defaultRoot() : m_setup.cacheRoot,
            m_setup.hashesPath.isEmpty() ? FileHashes::defaultPath() : m_setup.hashesPath);
        m_process->environment.models = &m_models;
        m_thumbnails = m_setup.thumbnailDir.isEmpty() ? cacheDir() + QStringLiteral("/thumbnails") : m_setup.thumbnailDir;
        m_lock = std::make_unique<Lock>(m_context.lockPath);
        m_poll = new QTimer(this);
        m_poll->setInterval(50);
        connect(m_poll, &QTimer::timeout, this, &RunThread::poll);
    }

    // Called while the thread still runs, maybe inside checkGraph's own event loop: ends the
    // runs, and leaves what a check may still be using to teardown().
    void shutdown() {
        m_quitting = true;
        m_poll->stop();
        for (const Parked &parked : std::as_const(m_parked))
            emit event(parked.id, {{"event", "run.done"}, {"status", runStatusName(RunStatus::Cancelled)}});
        m_parked.clear();
        for (const Waiting &waiting : std::as_const(m_waiting))
            if (queue().remove(waiting.folder.id()))
                waiting.folder.finish(RunStatus::Cancelled, {}, now());
        m_waiting.clear();
        if (m_active) {
            m_active.reset(); // cancels and waits for its kernels
            queue().remove(m_activeId);
        }
        m_lock.reset();
    }

    // On the run thread once its event loop has ended (QThread::finished), so no check is
    // under way: the providers go with the thread they belong to.
    void teardown() {
        m_context.registry.setProviders({});
        m_compileRegistry.setProviders({});
        m_process.reset();
    }

    void start(const QString &id, const RunRequest &request) {
        if (m_quitting)
            return;
        // checkGraph waits on providers with an event loop of its own, which delivers what is
        // queued here meanwhile: a start then waits for the check to end (compiling would
        // replace the providers the check is using).
        if (m_checking) {
            m_parked.append({id, request});
            return;
        }
        const Compilation compilation = compileRequest(request);
        if (!compilation.ok()) {
            emit event(id, {{"event", "run.done"}, {"status", runStatusName(RunStatus::Failed)},
                            {"errors", toJson(compilation.errors())}});
            return;
        }
        const Result<RunFolder> folder = RunFolder::create(m_context.runsRoot, id);
        if (!folder) {
            emit event(id, {{"event", "run.done"}, {"status", runStatusName(RunStatus::Failed)},
                            {"errors", toJson(folder.errors())}});
            return;
        }
        Errors written;
        if (!folder->writeCompiled(compilation->toJson()))
            written << makeError("permission_denied", "The compiled graph could not be written to " + folder->path() + ".");
        written += folder->writeWorkflow(request.document.toJson());
        if (m_setup.roots)
            written += folder->writeRoots(*m_setup.roots);
        if (!hasErrors(written) && !queue().enqueueForThisProcess(id, request.workflow, request.first))
            written << makeError("internal", "The run queue could not be written.", "Check " + m_context.queueDir);
        if (hasErrors(written)) {
            queue().remove(id);
            emit event(id, folder->finish(RunStatus::Failed, written, now()));
            return;
        }
        folder->queued(request.workflow);
        emit event(id, {{"event", "run.queued"}});
        Waiting waiting{folder.value(), compilation.value(), {}};
        waiting.options.workflow = request.workflow;
        waiting.options.document = request.document.toJson();
        waiting.options.roots = m_setup.roots;
        waiting.options.warnings = compilation.problems();
        m_waiting.append(waiting);
        m_poll->start();
        poll();
    }

    void cancel(const QString &id) {
        for (int i = 0; i < m_parked.size(); ++i) {
            if (m_parked.at(i).id == id) {
                m_parked.removeAt(i);
                emit event(id, {{"event", "run.done"}, {"status", runStatusName(RunStatus::Cancelled)}});
                return;
            }
        }
        if (m_active && m_activeId == id) {
            m_active->cancel();
            return;
        }
        for (int i = 0; i < m_waiting.size(); ++i) {
            if (m_waiting.at(i).folder.id() != id)
                continue;
            const RunFolder folder = m_waiting.takeAt(i).folder;
            QueueEntry entry;
            // Whoever takes the entry out ends the run; when someone else did, they did.
            if (queue().find(id, &entry) && queue().remove(entry))
                emit event(id, folder.finish(RunStatus::Cancelled, {}, now()));
            else
                emit event(id, ended(folder));
            return;
        }
    }

    // A thumbnail of an input file (Load Image before it runs): what the run could read only.
    void previewFile(const QString &token, const QString &path) {
        if (m_quitting)
            return;
        QString file = expandHome(path);
        if (m_setup.roots) {
            const Result<QString> allowed = m_setup.roots->checkRead(file, QStringLiteral("file"));
            if (!allowed) {
                emit filePreviewed(token, path, {{"missing", true}});
                return;
            }
            file = allowed.value();
        }
        const QFileInfo info(file);
        QJsonObject t;
        if (info.isFile() && info.isReadable())
            t = thumbnailOf(QJsonObject{{"path", info.absoluteFilePath()}}, m_thumbnails);
        emit filePreviewed(token, path, t.isEmpty() ? QJsonObject{{"missing", true}} : t);
    }

    void check(const RunRequest &request, int ticket) {
        m_check = request;
        m_checkTicket = ticket;
        if (!m_active && !m_checking)
            QTimer::singleShot(0, this, &RunThread::checkNow);
    }

signals:
    void event(const QString &run, const QJsonObject &event);
    void checked(int ticket, const QJsonObject &nodes, const QJsonArray &problems);
    void filePreviewed(const QString &token, const QString &path, const QJsonObject &thumbnail);

private:
    struct Waiting {
        RunFolder folder;
        CompiledGraph graph;
        RunOptions options;
    };

    Queue queue() const { return Queue(m_context.queueDir); }

    // Compiles as `fugue run` does (cli::compileDocument), with the providers from the setup,
    // into the registry runs do not use, so a run under way keeps its providers.
    Compilation compileRequest(const RunRequest &request) {
        const Errors providerProblems = m_setup.registerProviders(m_compileRegistry);
        if (hasErrors(providerProblems))
            return Compilation(providerProblems);
        CompileOptions options;
        options.target = request.target;
        options.providers = m_compileRegistry.providerNames();
        const Compilation compilation = compile(request.document, m_compileRegistry, options);
        const Errors problems = providerProblems + compilation.problems();
        return compilation ? Compilation(compilation.value(), problems) : Compilation(problems);
    }

    // The run.done of a run someone else ended (a `fugue runs cancel` of a waiting run).
    static QJsonObject ended(const RunFolder &folder) {
        for (const QJsonObject &e : folder.events())
            if (e.value("event") == QLatin1String("run.done"))
                return e;
        return {{"event", "run.done"}, {"status", runStatusName(folder.status())}};
    }

    void poll() {
        if (m_checking)
            return; // its turn is looked at again once the check is over
        if (m_active) {
            if (!m_cancelAsked && m_activeFolder && m_activeFolder->cancelRequested()) {
                m_cancelAsked = true;
                m_active->cancel();
            }
            return;
        }
        if (m_waiting.isEmpty()) {
            m_poll->stop();
            return;
        }
        cli::endDeadRuns(m_context, *m_lock);
        const QList<QueueEntry> entries = queue().entries();
        // A waiting run whose entry is gone was cancelled from outside, which ended it.
        for (int i = m_waiting.size() - 1; i >= 0; --i) {
            const QString id = m_waiting.at(i).folder.id();
            if (std::none_of(entries.begin(), entries.end(), [&](const QueueEntry &e) { return e.run == id; }))
                emit event(id, ended(m_waiting.takeAt(i).folder));
        }
        if (m_waiting.isEmpty() || entries.isEmpty())
            return;
        const QString first = entries.first().run;
        const auto mine = std::find_if(m_waiting.begin(), m_waiting.end(),
                                       [&](const Waiting &w) { return w.folder.id() == first; });
        if (mine == m_waiting.end() || !m_lock->tryAcquire() || !queue().activate(first))
            return;
        begin(m_waiting.takeAt(int(mine - m_waiting.begin())));
    }

    void begin(Waiting waiting) {
        // providers.json is read now, when the run's turn has come, as the worker does.
        const Errors providerProblems = m_setup.registerProviders(m_context.registry);
        const QString id = waiting.folder.id();
        if (hasErrors(providerProblems)) {
            queue().remove(id);
            emit event(id, waiting.folder.finish(RunStatus::Failed, providerProblems, now()));
            m_lock->release();
            return;
        }
        if (m_setup.collectGarbage) {
            cli::collectGarbageIfDue(m_context, *m_process);
            pruneThumbnails();
        }
        waiting.options.warnings += providerProblems;
        m_activeId = id;
        m_activeFolder = waiting.folder;
        m_cancelAsked = false;
        m_active.reset(new Run(waiting.graph, waiting.folder, m_process->environment, waiting.options));
        connect(m_active.get(), &Run::emitted, this, [this, id](const QJsonObject &e) { emit event(id, withThumbnails(e)); });
        connect(m_active.get(), &Run::finished, this, [this, id] {
            // shutdown() deletes the run, which cancels it and finishes on the way: shutdown
            // clears up after it, and m_active is already empty.
            if (m_quitting)
                return;
            queue().remove(id);
            m_active.release()->deleteLater();
            m_activeId.clear();
            m_lock->release();
            if (m_checkTicket >= 0)
                QTimer::singleShot(0, this, &RunThread::checkNow);
            poll();
        });
        m_active->start();
    }

    // Thumbnails are copies, made again when a result is shown and its copy is gone, so the
    // ones not made for 7 days go, at most once an hour.
    void pruneThumbnails() {
        if (m_pruned.isValid() && m_pruned.elapsed() < 3600 * 1000)
            return;
        m_pruned.start();
        const QDateTime before = QDateTime::currentDateTime().addDays(-7);
        const QFileInfoList files = QDir(m_thumbnails).entryInfoList(QDir::Files);
        for (const QFileInfo &file : files)
            if (file.lastModified() < before)
                QFile::remove(file.filePath());
    }

    QJsonObject withThumbnails(QJsonObject e) {
        const QString kind = e.value("event").toString();
        QJsonObject thumbnails;
        if (kind == QLatin1String("node.preview")) {
            QJsonObject image = e.value("image").toObject();
            image.insert("type", "image");
            // A preview frame has no content hash and comes many times a run: its copies go in
            // the run folder, which is collected with the run (10.5).
            const QString dir = m_activeFolder ? m_activeFolder->path() + QStringLiteral("/previews") : m_thumbnails;
            if (const QJsonObject t = thumbnailOf(image, dir); !t.isEmpty())
                thumbnails.insert("preview", t);
        } else if (kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")) {
            for (const char *part : {"outputs", "shown"}) {
                const QJsonObject values = e.value(QLatin1String(part)).toObject();
                for (auto it = values.begin(); it != values.end(); ++it) {
                    QJsonObject t = isImage(it.value()) ? thumbnailOf(it.value().toObject(), m_thumbnails)
                                                        : mediaLabel(it.value().toObject());
                    if (t.isEmpty())
                        continue;
                    // The file a sink wrote (Save Image): its name goes under the picture.
                    if (it.value().toObject().contains("path"))
                        t.insert("file", QFileInfo(it.value().toObject().value("path").toString()).fileName());
                    thumbnails.insert(it.key(), t);
                }
            }
        }
        if (!thumbnails.isEmpty())
            e.insert("thumbnails", thumbnails);
        return e;
    }

    void checkNow() {
        if (m_active || m_checking || m_quitting || m_checkTicket < 0)
            return;
        m_checking = true;
        const int ticket = m_checkTicket;
        m_checkTicket = -1;
        const Compilation compilation = compileRequest(m_check);
        QJsonObject nodes;
        Errors problems = compilation.problems();
        if (compilation.ok()) {
            RunEnvironment environment = m_process->environment;
            environment.registry = &m_compileRegistry;
            RunOptions options;
            options.roots = m_setup.roots;
            const GraphCheck graphCheck = checkGraph(*compilation, environment, options);
            problems += graphCheck.warnings;
            for (auto it = graphCheck.states.begin(); it != graphCheck.states.end(); ++it)
                nodes.insert(it.key(), QJsonObject{{"state", nodeStateName(it.value())}});
            for (auto it = compilation->skipped.begin(); it != compilation->skipped.end(); ++it)
                nodes.insert(it.key(), QJsonObject{{"state", nodeStateName(NodeState::Skipped)}, {"reason", it.value()}});
        }
        m_checking = false;
        if (m_quitting)
            return;
        emit checked(ticket, nodes, toJson(problems));
        // What waited for the check: runs started meanwhile, a newer check, the queue.
        const QList<Parked> parked = std::exchange(m_parked, {});
        for (const Parked &p : parked)
            start(p.id, p.request);
        if (m_checkTicket >= 0 && !m_active)
            QTimer::singleShot(0, this, &RunThread::checkNow);
        if (!m_waiting.isEmpty())
            poll();
    }

    RunSetup m_setup;
    cli::Context m_context;
    Registry m_compileRegistry;
    std::unique_ptr<cli::Process> m_process;
    ResidentModels m_models;
    QString m_thumbnails;
    std::unique_ptr<Lock> m_lock;
    QTimer *m_poll = nullptr;
    QList<Waiting> m_waiting;
    std::unique_ptr<Run> m_active;
    QString m_activeId;
    std::optional<RunFolder> m_activeFolder;
    bool m_cancelAsked = false;
    struct Parked {
        QString id;
        RunRequest request;
    };
    QList<Parked> m_parked; // started while a check was under way
    bool m_checking = false;
    QElapsedTimer m_pruned;
    bool m_quitting = false;
    RunRequest m_check;
    int m_checkTicket = -1;
};

RunService::RunService(RunSetup setup, QObject *parent)
    : QObject(parent), m_thread(new QThread(this)), m_worker(new RunThread(std::move(setup))) {
    qRegisterMetaType<fugue::RunOutcome>();
    m_thread->setObjectName(QStringLiteral("fugue-run"));
    m_worker->moveToThread(m_thread);
    connect(m_worker, &RunThread::event, this, &RunService::event);
    connect(m_worker, &RunThread::checked, this, &RunService::checked);
    connect(m_worker, &RunThread::filePreviewed, this, &RunService::filePreviewed);
    m_thread->start();
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->init(); }, Qt::BlockingQueuedConnection);
}

RunService::~RunService() {
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->shutdown(); }, Qt::BlockingQueuedConnection);
    // quit() also ends a check's own event loop; the worker goes on its thread once that is
    // over (finished, connected directly, comes from the run thread itself).
    connect(m_thread, &QThread::finished, m_worker, [w = m_worker] { w->teardown(); delete w; }, Qt::DirectConnection);
    m_thread->quit();
    m_thread->wait();
}

QString RunService::start(const RunRequest &request) {
    const QString id = newRunId();
    QMetaObject::invokeMethod(m_worker, [w = m_worker, id, request] { w->start(id, request); }, Qt::QueuedConnection);
    return id;
}

void RunService::cancel(const QString &run) {
    QMetaObject::invokeMethod(m_worker, [w = m_worker, run] { w->cancel(run); }, Qt::QueuedConnection);
}

void RunService::check(const RunRequest &request, int ticket) {
    QMetaObject::invokeMethod(m_worker, [w = m_worker, request, ticket] { w->check(request, ticket); },
                              Qt::QueuedConnection);
}

void RunService::previewFile(const QString &token, const QString &path) {
    QMetaObject::invokeMethod(m_worker, [w = m_worker, token, path] { w->previewFile(token, path); },
                              Qt::QueuedConnection);
}

void RunService::onRunThread(const std::function<void()> &call) {
    QMetaObject::invokeMethod(m_worker, [call] { call(); }, Qt::BlockingQueuedConnection);
}

} // namespace fugue

#include "run_service.moc"
