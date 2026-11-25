#include "run/run_service.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <memory>
#include <utility>

#include "cli/process.h"
#include "cli/runs.h"
#include "engine/compile_for_run.h"
#include "engine/compiler.h"
#include "engine/trust_store.h"
#include "engine/file_hashes.h"
#include "engine/paths.h"
#include "engine/cache_manager.h"
#include "engine/queue.h"
#include "engine/remote_consent.h"
#include "cli/compile_step.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"
#include "engine/scheduler.h"

namespace fugue {

namespace {

constexpr int kThumbnailWidth = 512;
// How long a closing window waits for its active run to end after cancelling it.
constexpr int kShutdownWaitMs = 5000;

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

// What stands in for a thumbnail of a video or audio result: a label, a video's shape, and the file
// for the player on the node (a first frame would need ffmpeg per result; the player shows it).
QJsonObject mediaLabel(const QJsonObject &handle) {
    const QString type = handle.value("type").toString();
    if (type != QLatin1String("video") && type != QLatin1String("audio"))
        return {};
    const int seconds = qRound(handle.value("duration").toDouble());
    const QString length = QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    // `mediaPath` is the file itself, which the node's player plays (a thumbnail's `source` is for pictures).
    QJsonObject label{{"label", (type == QLatin1String("video") ? RunService::tr("Video") : RunService::tr("Audio"))
                                    + QStringLiteral(" \u00b7 ") + length},
                      {"type", type},
                      {"mediaPath", handle.value("path").toString()},
                      {"duration", handle.value("duration").toDouble()}};
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
        m_thumbnails = m_setup.thumbnailDir.isEmpty() ? thumbnailsDir() : m_setup.thumbnailDir;
        m_lock = std::make_unique<Lock>(m_context.lockPath, Queue::crossRootLockFor(m_context.lockPath));
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
        if (m_active)
            endActiveRun();
        m_lock.reset();
    }

    // The active run, cancelled as the window closes: given kShutdownWaitMs to end as a run
    // does (its provider jobs cancelled, run.done written, the file-hash memo saved), else
    // deleted (which waits for its kernels) and ended here, so its folder never stays
    // "running" with no one left to end it.
    void endActiveRun() {
        if (!m_active->isFinished()) {
            QEventLoop loop;
            connect(m_active.get(), &Run::finished, &loop, &QEventLoop::quit);
            QTimer::singleShot(kShutdownWaitMs, &loop, &QEventLoop::quit);
            m_active->cancel();
            if (!m_active->isFinished())
                loop.exec();
        }
        const bool finished = m_active->isFinished();
        m_active.reset();
        if (!finished) {
            if (m_activeFolder && !isTerminal(m_activeFolder->status()))
                m_activeFolder->finish(RunStatus::Cancelled, {}, now());
            m_process->hashes.save();
        }
        queue().remove(m_activeId);
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
        // replace the providers the check is using), which it ends at once. A check waits
        // on describe() (a paired host: up to its transfer timeout); a run does not.
        if (m_checking) {
            m_parked.append({id, request});
            if (m_checkAbort)
                m_checkAbort->abort();
            return;
        }
        const Compilation compilation = compileRequest(request);
        if (!compilation.ok()) {
            emit event(id, {{"event", "run.done"}, {"status", runStatusName(RunStatus::Failed)},
                            {"errors", toJson(compilation.errors())}});
            return;
        }
        if (askRemote(id, request, compilation.value()))
            return;
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
        Waiting waiting{folder.value(), compilation.value(), {}, request.noPictures};
        waiting.options.workflow = request.workflow;
        waiting.options.document = request.document.toJson();
        waiting.options.roots = m_setup.roots;
        waiting.options.warnings = compilation.problems();
        m_waiting.append(waiting);
        m_poll->start();
        poll();
    }

    // Before a run is queued: when some instance is off-machine, looks at the graph as the run would (a check) and, when it
    // would send data to one the person has not allowed, keeps the request and asks (remoteConsentNeeded). True when it asks.
    // The run is the same run (its id) once answered. A check that was aborted says nothing: the scheduler refuses at the node.
    bool askRemote(const QString &id, const RunRequest &request, const CompiledGraph &graph) {
        const QList<ProviderInstance> instances = cli::registeredInstances(m_compileRegistry);
        if (!anyOffMachine(instances))
            return false;
        RemoteConsent consent(consentPath());
        consent.prune(instances);
        RunEnvironment environment = m_process->environment;
        environment.registry = &m_compileRegistry;
        RunOptions options;
        options.workflow = request.workflow;
        options.roots = m_setup.roots;
        CheckAbort abort;
        m_checking = true;
        m_checkAbort = &abort;
        const GraphCheck check = checkGraph(graph, environment, options, &abort);
        m_checkAbort = nullptr;
        m_checking = false;
        QSet<QString> runs;
        for (auto it = check.states.cbegin(); it != check.states.cend(); ++it)
            if (it.value() == NodeState::Stale)
                runs.insert(it.key());
        const QList<RemoteUse> uses = notAllowed(remoteUses(check.routes, runs, instances), consent.allowedNames(instances));
        if (!check.aborted && !uses.isEmpty()) {
            QJsonArray services;
            QStringList names;
            for (const RemoteUse &use : uses) {
                names << use.instance;
                services.append(QJsonObject{{"instance", use.instance}, {"host", use.host}, {"sends", QJsonArray::fromStringList(use.sends)},
                                            {"text", sendsText(use.sends)}});
            }
            m_asking.insert(id, Asking{request, names});
            emit remoteConsentNeeded(id, services);
        }
        settleAfterCheck();
        return !check.aborted && !uses.isEmpty();
    }

    QString consentPath() const { return m_setup.consentPath.isEmpty() ? RemoteConsent::defaultPath() : m_setup.consentPath; }

    // The person's answer: yes keeps the answer for the instances asked about and starts the run; no ends it as cancelled.
    void answerRemote(const QString &id, bool allow) {
        const auto asked = m_asking.find(id);
        if (asked == m_asking.end())
            return;
        const Asking asking = asked.value();
        m_asking.erase(asked);
        if (!allow) {
            emit event(id, {{"event", "run.done"}, {"status", runStatusName(RunStatus::Cancelled)}});
            return;
        }
        RemoteConsent consent(consentPath());
        for (const ProviderInstance &instance : cli::registeredInstances(m_compileRegistry))
            if (asking.instances.contains(instance.name))
                consent.allow(instance);
        start(id, asking.request);
    }

    void cancel(const QString &id) {
        if (m_asking.contains(id)) {
            m_asking.remove(id);
            emit event(id, {{"event", "run.done"}, {"status", runStatusName(RunStatus::Cancelled)}});
            return;
        }
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
        // CanvasRuns has taken a relative path from the workflow's folder; one still relative
        // (an unsaved workflow's) names nothing yet.
        QString file = expandHome(path);
        if (!QDir::isAbsolutePath(file)) {
            emit filePreviewed(token, path, {{"missing", true}});
            return;
        }
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

    // A finished run's results again, from its run folder: its node.done, node.cached, node.skipped and
    // node.error events and its run.done, with the thumbnails made as a live run's are (the queue
    // panel's history, "show its results"). No node.running or node.progress: nothing is going.
    void replay(const QString &id) {
        if (m_quitting)
            return;
        const Result<RunFolder> folder = RunFolder::open(m_context.runsRoot, id);
        if (!folder)
            return;
        static const QStringList kinds{"node.done", "node.cached", "node.skipped", "node.error", "run.done"};
        for (const QJsonObject &e : folder->events())
            if (kinds.contains(e.value("event").toString()))
                emit event(id, withThumbnails(e));
    }

    // Called from the window's thread, not queued: the count must be up when this returns, so a run
    // whose start is already under way on this thread (compiled, not yet begun) sees the reload at
    // its turn and fails as changed, instead of the reload waiting in the queue behind it (#223).
    void reloadNodeTypes() { m_typesWanted.fetch_add(1); }

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
    // A run would send data to off-machine services the person has not allowed (8.2, #404): `services` is
    // [{"instance", "host", "sends": [..], "text"}]. The run waits for answerRemote.
    void remoteConsentNeeded(const QString &run, const QJsonArray &services);

private:
    struct Waiting {
        RunFolder folder;
        CompiledGraph graph;
        RunOptions options;
        QSet<QString> noPictures; // path ids whose thumbnails are not made (RunRequest::noPictures)
    };

    Queue queue() const { return Queue(m_context.queueDir); }

    // The nodes of a waiting run whose type is gone, is at another version than it was compiled
    // to, or comes from a pack that is not trusted at the version installed now (6.7): a pack
    // updated, removed or replaced while the run waited (#70).
    Errors changedSinceCompiled(const CompiledGraph &graph) const {
        Errors errors;
        for (auto it = graph.nodes.cbegin(); it != graph.nodes.cend(); ++it) {
            const NodeType *type = m_context.registry.nodeTypeAt(it->type, it->version.major);
            const QString pack = TrustStore::packOf(it->type);
            const QString version = type && !type->packVersion.isEmpty() ? type->packVersion : m_context.registry.packVersion(pack);
            Error error;
            if (!type || type->version != it->version)
                error = makeError(QStringLiteral("node_version_mismatch"),
                                  QStringLiteral("%1 (%2) changed while the run waited: its pack was updated or removed.")
                                      .arg(it.key(), it->type),
                                  QStringLiteral("Run again to use what is installed now."));
            else if (!TrustStore::isBuiltIn(pack) && !m_trust.isTrusted(pack, version))
                error = makeError(QStringLiteral("pack_untrusted"),
                                  QStringLiteral("Node %1 is from the pack %2 %3, which you have not allowed to run.")
                                      .arg(it.key(), pack, version),
                                  QStringLiteral("Trust %1 on the Packs page, or run `fugue pack trust %1`.").arg(pack));
            else
                continue;
            error.node = it.key();
            errors << error;
        }
        return errors;
    }

    // Fills `registry` again when a reload was asked for since `filled`; called only where nothing
    // uses it (compileRequest: no check under way; begin: no run active).
    void refill(Registry &registry, int &filled) {
        const int wanted = m_typesWanted.load();
        if (filled == wanted || !m_setup.addNodeTypes)
            return;
        Registry fresh;
        m_setup.addNodeTypes(fresh);
        registry.replaceTypes(std::move(fresh));
        filled = wanted;
    }

    // Compiles as `fugue run` does (cli::compileDocument), with the providers from the setup,
    // into the registry runs do not use, so a run under way keeps its providers.
    Compilation compileRequest(const RunRequest &request) {
        refill(m_compileRegistry, m_compileTypes);
        return compileForRun(request.document, m_compileRegistry, {request.target, {}, request.entered},
                             runCompileSetup(m_setup.registerProviders, m_trust, m_setup.activeCanvas));
    }

    // The run.done of a waiting run whose entry someone else took: only a cancel does that
    // (`fugue runs cancel`), which takes the entry and then ends the folder. One not ended yet
    // (or whose canceller died in between) is ended here as cancelled; RunFolder::finish
    // writes one run.done whoever comes first.
    static QJsonObject ended(const RunFolder &folder) {
        return folder.finish(RunStatus::Cancelled, {}, now());
    }

    void poll() {
        if (m_checking) {
            // A check under way gives way to a run whose turn has come, which would otherwise
            // hold up every run queued behind it too; the run starts once the check is over.
            const QList<QueueEntry> entries = queue().entries();
            if (m_checkAbort && !entries.isEmpty()
                && std::any_of(m_waiting.cbegin(), m_waiting.cend(),
                               [&](const Waiting &w) { return w.folder.id() == entries.first().run; }))
                m_checkAbort->abort();
            return;
        }
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
        if (mine == m_waiting.end() || !m_lock->tryAcquire())
            return;
        if (!queue().activate(first)) {
            // Its entry went meanwhile (a `fugue runs cancel`): the lock goes back at once, or
            // every worker would wait on a window that runs nothing. The next poll ends the run.
            m_lock->release();
            return;
        }
        begin(m_waiting.takeAt(int(mine - m_waiting.begin())));
    }

    void begin(Waiting waiting) {
        // The types too, after a reload: no run is active, so nothing holds them.
        refill(m_context.registry, m_runTypes);
        const QString id = waiting.folder.id();
        // A reload while it waited may have changed what it was compiled to: it never runs
        // what it was not compiled and trusted for.
        if (const Errors changed = changedSinceCompiled(waiting.graph); hasErrors(changed)) {
            queue().remove(id);
            emit event(id, waiting.folder.finish(RunStatus::Failed, changed, now()));
            m_lock->release();
            return;
        }
        // providers.json is read now, when the run's turn has come, as the worker does.
        const Errors providerProblems = m_setup.registerProviders(m_context.registry);
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
        waiting.options.remoteAllowed = RemoteConsent(consentPath()).allowedNames(cli::registeredInstances(m_context.registry));
        // The limit may have changed in Settings since the last run (10.6).
        m_process->cache.setLimit(CachePolicy::current().resultsBytes);
        m_activeId = id;
        m_activeFolder = waiting.folder;
        m_activeNoPictures = waiting.noPictures;
        m_cancelAsked = false;
        m_active.reset(new Run(waiting.graph, waiting.folder, m_process->environment, waiting.options));
        connect(m_active.get(), &Run::emitted, this, [this, id](const QJsonObject &e) { emit event(id, withThumbnails(e, m_activeNoPictures)); });
        connect(m_active.get(), &Run::finished, this, [this, id] {
            // A run that ends while the window closes (endActiveRun waits for this) is
            // cleared up there.
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
    // ones not shown for 7 days go, and the oldest past the limit (10.6), at most once an hour.
    // The run's turn has come and its engine lock is held, so the cleaning takes none.
    void pruneThumbnails() {
        if (m_pruned.isValid() && m_pruned.elapsed() < 3600 * 1000)
            return;
        m_pruned.start();
        CacheFolders folders;
        folders.thumbnails = m_thumbnails;
        if (m_setup.thumbnailDir.isEmpty())
            folders.mediaThumbnails = mediaThumbnailsDir(); // one limit for both (the run's turn holds the lock)
        CacheManager(folders).cleanupLocked(CachePolicy::current());
    }

    // `skip`: the nodes whose pictures the canvas does not show, which cost no thumbnail (#263).
    QJsonObject withThumbnails(QJsonObject e, const QSet<QString> &skip = {}) {
        const QString kind = e.value("event").toString();
        QJsonObject thumbnails;
        // A live frame is made for a hidden picture too: the card shows it while the node runs.
        if (kind != QLatin1String("node.preview") && skip.contains(e.value("node").toString()))
            return e;
        if (kind == QLatin1String("node.preview")) {
            QJsonObject image = e.value("image").toObject();
            image.insert("type", "image");
            // A preview frame has no content hash and comes many times a run: its copies go in
            // the run folder, which is collected with the run (10.5).
            const QString dir = m_activeFolder ? m_activeFolder->path() + QStringLiteral("/previews") : m_thumbnails;
            if (const QJsonObject t = thumbnailOf(image, dir); !t.isEmpty())
                thumbnails.insert("preview", t);
        } else if (kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")) {
            // A sink with outputs (Compare Images) shows its inputs beside them: those thumbnails
            // come as `inputThumbnails`, so they never stand in for an output's.
            QJsonObject inputThumbnails;
            const bool hasOutputs = !e.value("outputs").toObject().isEmpty();
            for (const char *part : {"outputs", "shown"}) {
                const QJsonObject values = e.value(QLatin1String(part)).toObject();
                QJsonObject &into = hasOutputs && QLatin1String(part) == QLatin1String("shown") ? inputThumbnails : thumbnails;
                for (auto it = values.begin(); it != values.end(); ++it) {
                    QJsonObject t = isImage(it.value()) ? thumbnailOf(it.value().toObject(), m_thumbnails)
                                                        : mediaLabel(it.value().toObject());
                    if (t.isEmpty())
                        continue;
                    // The file a sink wrote (Save Image): its name goes under the picture.
                    if (it.value().toObject().contains("path"))
                        t.insert("file", QFileInfo(it.value().toObject().value("path").toString()).fileName());
                    into.insert(it.key(), t);
                }
            }
            if (!inputThumbnails.isEmpty())
                e.insert("inputThumbnails", inputThumbnails);
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
        bool aborted = false;
        if (compilation.ok()) {
            RunEnvironment environment = m_process->environment;
            environment.registry = &m_compileRegistry;
            RunOptions options;
            options.workflow = m_check.workflow; // a relative file is its folder's (Appendix A)
            options.roots = m_setup.roots;
            CheckAbort abort;
            m_checkAbort = &abort;
            const GraphCheck graphCheck = checkGraph(*compilation, environment, options, &abort);
            m_checkAbort = nullptr;
            aborted = graphCheck.aborted;
            problems += graphCheck.warnings;
            const QList<ProviderInstance> instances = cli::registeredInstances(m_compileRegistry);
            for (auto it = graphCheck.states.begin(); it != graphCheck.states.end(); ++it) {
                QJsonObject node{{"state", nodeStateName(it.value())}};
                // A node routed to an off-machine service says so on its face (#404).
                const auto route = graphCheck.routes.constFind(it.key());
                if (route != graphCheck.routes.cend())
                    for (const ProviderInstance &instance : instances)
                        if (instance.name == route->instance && placementOf(instance) == Placement::OffMachine)
                            node.insert("runsOn", QStringLiteral("%1 (%2)").arg(instance.name, hostOf(instance)));
                nodes.insert(it.key(), node);
            }
            for (auto it = compilation->skipped.begin(); it != compilation->skipped.end(); ++it)
                nodes.insert(it.key(), QJsonObject{{"state", nodeStateName(NodeState::Skipped)}, {"reason", it.value()}});
        }
        m_checking = false;
        if (m_quitting)
            return;
        // An aborted check is made again later (once no run is active), unless a newer one
        // replaced it.
        if (!aborted)
            emit checked(ticket, nodes, toJson(problems));
        else if (m_checkTicket < 0)
            m_checkTicket = ticket;
        settleAfterCheck();
    }

    // What waited for a check: runs started meanwhile, the queue, a newer check.
    void settleAfterCheck() {
        const QList<Parked> parked = std::exchange(m_parked, {});
        for (const Parked &p : parked)
            start(p.id, p.request);
        if (!m_waiting.isEmpty())
            poll();
        if (m_checkTicket >= 0 && !m_active)
            QTimer::singleShot(0, this, &RunThread::checkNow);
    }

    RunSetup m_setup;
    TrustStore m_trust{m_setup.trustPath.isEmpty() ? TrustStore::defaultPath() : m_setup.trustPath};
    cli::Context m_context;
    Registry m_compileRegistry;
    // Reloads asked for, and the one each registry was last filled for.
    std::atomic<int> m_typesWanted{0};
    int m_compileTypes = 0;
    int m_runTypes = 0;
    std::unique_ptr<cli::Process> m_process;
    ResidentModels m_models;
    QString m_thumbnails;
    std::unique_ptr<Lock> m_lock;
    QTimer *m_poll = nullptr;
    QList<Waiting> m_waiting;
    std::unique_ptr<Run> m_active;
    QString m_activeId;
    std::optional<RunFolder> m_activeFolder;
    QSet<QString> m_activeNoPictures; // of the run in progress
    bool m_cancelAsked = false;
    struct Parked {
        QString id;
        RunRequest request;
    };
    struct Asking {
        RunRequest request;
        QStringList instances;
    };
    QMap<QString, Asking> m_asking; // runs waiting for the answer to the off-machine question (#404)
    QList<Parked> m_parked; // started while a check was under way
    bool m_checking = false;
    CheckAbort *m_checkAbort = nullptr; // the check under way's, while m_checking
    QElapsedTimer m_pruned;
    bool m_quitting = false;
    RunRequest m_check;
    int m_checkTicket = -1;
};

RunService::RunService(RunSetup setup, QObject *parent)
    : QObject(parent), m_runsRoot(setup.runsRoot.isEmpty() ? RunFolder::defaultRoot() : setup.runsRoot),
      m_thread(new QThread(this)), m_worker(new RunThread(std::move(setup))) {
    qRegisterMetaType<fugue::RunOutcome>();
    m_thread->setObjectName(QStringLiteral("fugue-run"));
    m_worker->moveToThread(m_thread);
    connect(m_worker, &RunThread::event, this, &RunService::event);
    connect(m_worker, &RunThread::checked, this, &RunService::checked);
    connect(m_worker, &RunThread::filePreviewed, this, &RunService::filePreviewed);
    connect(m_worker, &RunThread::remoteConsentNeeded, this, &RunService::remoteConsentNeeded);
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

void RunService::answerRemote(const QString &run, bool allow) {
    QMetaObject::invokeMethod(m_worker, [w = m_worker, run, allow] { w->answerRemote(run, allow); }, Qt::QueuedConnection);
}

void RunService::check(const RunRequest &request, int ticket) {
    QMetaObject::invokeMethod(m_worker, [w = m_worker, request, ticket] { w->check(request, ticket); },
                              Qt::QueuedConnection);
}

void RunService::previewFile(const QString &token, const QString &path) {
    QMetaObject::invokeMethod(m_worker, [w = m_worker, token, path] { w->previewFile(token, path); },
                              Qt::QueuedConnection);
}

void RunService::replay(const QString &run) {
    QMetaObject::invokeMethod(m_worker, [w = m_worker, run] { w->replay(run); }, Qt::QueuedConnection);
}

void RunService::reloadNodeTypes() {
    m_worker->reloadNodeTypes(); // thread-safe: see RunThread::reloadNodeTypes
}

void RunService::onRunThread(const std::function<void()> &call) {
    QMetaObject::invokeMethod(m_worker, [call] { call(); }, Qt::BlockingQueuedConnection);
}

} // namespace fugue

#include "run_service.moc"
