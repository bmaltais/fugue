#include "run/run_queue.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <ctime>
#include <unistd.h>

#include "cli/runs.h"
#include "engine/clock.h"
#include "engine/queue.h"
#include "engine/run_folder.h"
#include "run/run_service.h"

namespace fugue {

namespace {

qint64 bootNowMs() {
    timespec now{};
    ::clock_gettime(CLOCK_BOOTTIME, &now);
    return qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

QString nameOf(const QString &workflow) {
    return workflow.isEmpty() ? RunQueue::tr("Unsaved workflow") : QFileInfo(workflow).completeBaseName();
}

qint64 millisOf(const QString &iso) {
    const QDateTime time = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return time.isValid() ? time.toMSecsSinceEpoch() : 0;
}

// The node going and its progress, from the events so far: the last node.running, and the last
// node.progress for it.
void foldProgress(const QList<QJsonObject> &events, QString *node, qreal *progress) {
    *node = QString();
    *progress = -1;
    for (const QJsonObject &event : events) {
        const QString kind = event.value("event").toString();
        if (kind == QLatin1String("node.running")) {
            *node = event.value("node").toString();
            *progress = -1;
        } else if (kind == QLatin1String("node.progress") && event.value("node").toString() == *node) {
            *progress = event.value("value").toDouble(-1);
        } else if ((kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")
                    || kind == QLatin1String("node.error")) && event.value("node").toString() == *node) {
            *node = QString();
            *progress = -1;
        }
    }
}

} // namespace

RunQueue::RunQueue(RunQueuePaths paths, RunService *service, QObject *parent)
    : QObject(parent), m_paths(std::move(paths)), m_service(service) {
    if (m_paths.runsRoot.isEmpty())
        m_paths.runsRoot = RunFolder::defaultRoot();
    if (m_paths.queueDir.isEmpty())
        m_paths.queueDir = Queue::defaultDir();
    if (m_paths.lockPath.isEmpty())
        m_paths.lockPath = Queue::defaultLockPath();
    m_openUrl = [](const QUrl &url) { QDesktopServices::openUrl(url); };
    m_later.setSingleShot(true);
    m_later.setInterval(30);
    connect(&m_later, &QTimer::timeout, this, &RunQueue::refresh);
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &RunQueue::refresh);
    // The queue folder changes when anyone queues, starts or ends a run: no polling at idle.
    QDir().mkpath(m_paths.queueDir);
    m_watcher.addPath(m_paths.queueDir);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { schedule(); });
    if (m_service)
        connect(m_service, &RunService::event, this, [this](const QString &, const QJsonObject &event) {
            const QString kind = event.value("event").toString();
            if (kind == QLatin1String("run.queued") || kind == QLatin1String("run.started") || kind == QLatin1String("run.done"))
                schedule();
        });
    refresh();
}

void RunQueue::setWatching(bool watching) {
    if (m_watching == watching)
        return;
    m_watching = watching;
    if (watching)
        m_tick.start();
    else
        m_tick.stop();
    refresh(); // the history and the going run's progress are read only while watching
    emit watchingChanged();
}

void RunQueue::refresh() {
    const Queue queue(m_paths.queueDir);
    // A run whose worker died is ended before it is shown, so it does not read as going for
    // ever (what `fugue runs list` does).
    {
        Lock lock(m_paths.lockPath);
        cli::Context context;
        context.runsRoot = m_paths.runsRoot;
        context.queueDir = m_paths.queueDir;
        context.lockPath = m_paths.lockPath;
        cli::endDeadRuns(context, lock);
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 boot = bootNowMs();
    const qint64 self = qint64(::getpid());
    const qint64 started = processStartTime(self);

    QVariantList entries;
    QSet<QString> queued;
    int position = 0;
    for (const QueueEntry &entry : queue.entries()) {
        queued.insert(entry.run);
        const bool mine = entry.inProcess && entry.pid == self && entry.processStart == started;
        QString workflow = entry.workflow;
        const Result<RunFolder> folder = RunFolder::open(m_paths.runsRoot, entry.run);
        QString node;
        qreal progress = -1;
        qint64 since = entry.queuedBootMs > 0 ? now - (boot - entry.queuedBootMs) : now;
        if (folder) {
            const QJsonObject state = folder->state();
            if (workflow.isEmpty())
                workflow = state.value("workflow").toObject().value("path").toString();
            if (entry.active) {
                if (const qint64 at = millisOf(state.value("started").toString()); at > 0)
                    since = at;
                if (m_watching)
                    foldProgress(folder->events(), &node, &progress);
            }
        }
        entries.append(QVariantMap{{"run", entry.run},
                                   {"name", nameOf(workflow)},
                                   {"workflow", workflow},
                                   {"status", entry.active ? "running" : "waiting"},
                                   {"position", entry.active ? 0 : ++position},
                                   {"source", entry.inProcess ? "window" : "cli"},
                                   {"mine", mine},
                                   {"since", since},
                                   {"node", node},
                                   {"progress", progress}});
    }

    QVariantList history;
    if (m_watching) {
        const QStringList ids = QDir(m_paths.runsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
        int looked = 0;
        for (const QString &id : ids) {
            if (history.size() >= historyLimit || ++looked > 60)
                break;
            if (queued.contains(id))
                continue;
            const Result<RunFolder> folder = RunFolder::open(m_paths.runsRoot, id);
            if (!folder)
                continue;
            const QJsonObject state = folder->state();
            const RunStatus status = parseRunStatus(state.value("status").toString());
            if (!isTerminal(status))
                continue;
            const QString workflow = state.value("workflow").toObject().value("path").toString();
            const qint64 begun = millisOf(state.value("started").toString());
            const qint64 ended = millisOf(state.value("ended").toString());
            history.append(QVariantMap{{"run", id},
                                       {"name", nameOf(workflow)},
                                       {"workflow", workflow},
                                       {"status", runStatusName(status)},
                                       {"seconds", begun > 0 && ended >= begun ? (ended - begun) / 1000.0 : -1.0},
                                       {"ended", ended}});
        }
    }
    // `since` of a waiting run is worked out from two clocks read a moment apart: a millisecond of
    // that is no change, and a change makes the panel redraw its rows.
    for (QVariant &row : entries) {
        QVariantMap map = row.toMap();
        for (const QVariant &old : std::as_const(m_entries)) {
            const QVariantMap before = old.toMap();
            if (before.value("run") == map.value("run") && qAbs(before.value("since").toLongLong() - map.value("since").toLongLong()) < 2000) {
                map.insert("since", before.value("since"));
                row = map;
                break;
            }
        }
    }
    if (entries == m_entries && history == m_history)
        return;
    m_entries = entries;
    m_history = history;
    emit changed();
}

void RunQueue::cancel(const QString &run) {
    QueueEntry entry;
    const bool queued = Queue(m_paths.queueDir).find(run, &entry);
    if (queued && entry.inProcess && entry.pid == qint64(::getpid()) && m_service) {
        m_service->cancel(run);
        return;
    }
    const Result<RunFolder> folder = RunFolder::open(m_paths.runsRoot, run);
    if (!folder)
        return;
    cli::Context context;
    context.runsRoot = m_paths.runsRoot;
    context.queueDir = m_paths.queueDir;
    context.lockPath = m_paths.lockPath;
    cli::cancelRun(*folder, queued ? std::optional<QueueEntry>(entry) : std::nullopt, context);
    schedule();
}

bool RunQueue::moveToFront(const QString &run) {
    const bool moved = Queue(m_paths.queueDir).moveToFront(run);
    schedule();
    return moved;
}

void RunQueue::openFolder(const QString &run) {
    const Result<RunFolder> folder = RunFolder::open(m_paths.runsRoot, run);
    if (folder && m_openUrl)
        m_openUrl(QUrl::fromLocalFile(folder->path()));
}

QString RunQueue::workflowOf(const QString &run) const {
    const Result<RunFolder> folder = RunFolder::open(m_paths.runsRoot, run);
    return folder ? folder->state().value("workflow").toObject().value("path").toString() : QString();
}

} // namespace fugue
