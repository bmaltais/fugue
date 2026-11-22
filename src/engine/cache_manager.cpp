#include "engine/cache_manager.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QSet>

#include <algorithm>

#include <sys/stat.h>

#include "engine/json.h"
#include "engine/paths.h"
#include "engine/queue.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"
#include "engine/settings.h"

namespace fugue {

namespace {

// A run that never ended and is not in the queue is left alone for this long (it may be a
// `fugue run` that has not written its queue entry yet), then counts as abandoned.
constexpr qint64 kAbandonedAfterSeconds = 24 * 3600;

QDateTime parseIso(const QString &text) {
    const QDateTime time = QDateTime::fromString(text, Qt::ISODateWithMs);
    return time.isValid() ? time.toUTC() : QDateTime();
}

QJsonValue timeJson(const QDateTime &time) {
    return time.isValid() ? QJsonValue(isoTime(time)) : QJsonValue(QJsonValue::Null);
}

struct Walked {
    qint64 bytes = 0;
    int files = 0;
};

// The files under `path` (links are not followed). With `ownedOnly`, a file with another hard
// link counts for nothing: a run's copy of a stored result frees no space when it goes.
Walked walk(const QString &path, bool ownedOnly) {
    Walked walked;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        struct stat info;
        if (::lstat(QFile::encodeName(it.next()).constData(), &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        ++walked.files;
        if (!ownedOnly || info.st_nlink == 1)
            walked.bytes += info.st_size;
    }
    return walked;
}

struct PictureFile {
    QString path;
    qint64 bytes = 0;
    QDateTime changed;
};

// The previews, oldest first. A half-written file (".part") is not one yet.
QList<PictureFile> pictureFiles(const QString &folder) {
    QList<PictureFile> files;
    for (const QFileInfo &info : QDir(folder).entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
        if (!info.fileName().endsWith(QLatin1String(".part")))
            files.append({info.absoluteFilePath(), info.size(), info.lastModified().toUTC()});
    std::sort(files.begin(), files.end(), [](const PictureFile &a, const PictureFile &b) {
        return a.changed != b.changed ? a.changed < b.changed : a.path < b.path;
    });
    return files;
}

// The previews of both thumbnail folders, oldest first.
QList<PictureFile> thumbnailFiles(const CacheFolders &folders) {
    QList<PictureFile> files = pictureFiles(folders.thumbnails);
    files.append(pictureFiles(folders.mediaThumbnails));
    std::sort(files.begin(), files.end(), [](const PictureFile &a, const PictureFile &b) {
        return a.changed != b.changed ? a.changed < b.changed : a.path < b.path;
    });
    return files;
}

bool hasThumbnails(const CacheFolders &folders) { return !folders.thumbnails.isEmpty() || !folders.mediaThumbnails.isEmpty(); }

struct RunInfo {
    QString id;
    QString path;
    QString workflow;
    bool active = false;  // queued or going
    QDateTime when;       // when it ended, else when it started, else when the folder changed
    qint64 bytes = 0;
    int files = 0;
};

QList<RunInfo> listRuns(const CacheFolders &folders, const QSet<QString> &queued, const QDateTime &now) {
    QList<RunInfo> runs;
    const QDir root(folders.runs);
    for (const QString &id : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        RunInfo run;
        run.id = id;
        run.path = root.filePath(id);
        const Result<QJsonObject> read = readJsonObject(run.path + QStringLiteral("/run.json"));
        const QJsonObject state = read ? read.value() : QJsonObject();
        run.workflow = state.value("workflow").toObject().value("path").toString();
        RunStatus status = parseRunStatus(state.value("status").toString());
        QDateTime ended = parseIso(state.value("ended").toString());
        if (!isTerminal(status)) {
            // run.done is what decides (9.7): the events may say it ended when run.json was not yet written.
            if (const Result<RunFolder> folder = RunFolder::open(folders.runs, id)) {
                status = folder->status();
                if (isTerminal(status) && !ended.isValid())
                    for (const QJsonObject &event : folder->events())
                        if (event.value("event").toString() == QLatin1String("run.done"))
                            ended = parseIso(event.value("time").toString());
            }
        }
        const QDateTime started = parseIso(state.value("started").toString());
        const QDateTime changed = QFileInfo(run.path).lastModified().toUTC();
        run.when = ended.isValid() ? ended : started.isValid() ? started : changed;
        if (!isTerminal(status))
            run.active = queued.contains(id) || run.when.secsTo(now) < kAbandonedAfterSeconds;
        const Walked walked = walk(run.path, true);
        run.bytes = walked.bytes;
        run.files = walked.files;
        runs.append(run);
    }
    return runs;
}

bool removeFolder(const QString &path) { return QDir(path).removeRecursively(); }

} // namespace

QString cacheKindName(CacheKind kind) {
    switch (kind) {
    case CacheKind::Results: return QStringLiteral("results");
    case CacheKind::Thumbnails: return QStringLiteral("thumbnails");
    case CacheKind::Runs: return QStringLiteral("runs");
    }
    return {};
}

std::optional<CacheKind> parseCacheKind(const QString &name) {
    for (const CacheKind kind : allCacheKinds())
        if (name == cacheKindName(kind))
            return kind;
    return std::nullopt;
}

QList<CacheKind> allCacheKinds() { return {CacheKind::Results, CacheKind::Thumbnails, CacheKind::Runs}; }

CacheFolders CacheFolders::standard() {
    CacheFolders folders;
    folders.results = ResultCache::defaultRoot();
    folders.thumbnails = thumbnailsDir();
    folders.mediaThumbnails = mediaThumbnailsDir();
    folders.runs = RunFolder::defaultRoot();
    folders.queue = Queue::defaultDir();
    folders.lock = Queue::defaultLockPath();
    return folders;
}

CachePolicy CachePolicy::from(const Settings &settings) {
    CachePolicy policy;
    policy.resultsBytes = qint64(settings.cacheResultsGb * double(gigabyte));
    policy.thumbnailsBytes = qint64(settings.cacheThumbnailsGb * double(gigabyte));
    policy.runsKept = settings.runsKept;
    return policy;
}

CachePolicy CachePolicy::current() { return from(currentSettings()); }

QJsonObject KindUsage::toJson() const {
    return QJsonObject{{"kind", cacheKindName(kind)}, {"bytes", bytes},   {"files", files},
                       {"items", items},             {"oldest", timeJson(oldest)}, {"limit", limit},
                       {"protected", protectedItems}};
}

const KindUsage *CacheStatus::usage(CacheKind kind) const {
    for (const KindUsage &each : kinds)
        if (each.kind == kind)
            return &each;
    return nullptr;
}

qint64 CacheStatus::totalBytes() const {
    qint64 total = 0;
    for (const KindUsage &each : kinds)
        total += each.bytes;
    return total;
}

QJsonObject CacheStatus::toJson() const {
    QJsonArray kindsJson;
    for (const KindUsage &each : kinds)
        kindsJson.append(each.toJson());
    QJsonArray workflowsJson;
    for (const WorkflowUsage &workflow : workflows) {
        QJsonArray nodesJson;
        for (const NodeUsage &node : workflow.nodes)
            nodesJson.append(QJsonObject{{"node", node.node}, {"bytes", node.bytes}, {"entries", node.entries}});
        workflowsJson.append(QJsonObject{{"workflow", workflow.workflow},
                                         {"bytes", workflow.bytes},
                                         {"entries", workflow.entries},
                                         {"nodes", nodesJson}});
    }
    return QJsonObject{{"kinds", kindsJson}, {"total_bytes", totalBytes()}, {"workflows", workflowsJson}};
}

QJsonObject KindOutcome::toJson() const {
    QJsonObject json{{"kind", cacheKindName(kind)}, {"removed", removed}, {"bytes", bytes}, {"kept", kept}};
    if (!skipped.isEmpty())
        json.insert("skipped", skipped);
    return json;
}

qint64 ClearReport::bytes() const {
    qint64 total = 0;
    for (const KindOutcome &each : kinds)
        total += each.bytes;
    return total;
}

int ClearReport::removed() const {
    int total = 0;
    for (const KindOutcome &each : kinds)
        total += each.removed;
    return total;
}

QJsonObject ClearReport::toJson() const {
    QJsonArray kindsJson;
    for (const KindOutcome &each : kinds)
        kindsJson.append(each.toJson());
    return QJsonObject{{"dry_run", dryRun}, {"kinds", kindsJson}, {"bytes", bytes()}, {"removed", removed()}};
}

CacheManager::CacheManager(CacheFolders folders, Clock clock) : m_folders(std::move(folders)), m_clock(std::move(clock)) {}

QStringList CacheManager::activeRuns() const {
    QSet<QString> queued;
    if (!m_folders.queue.isEmpty())
        for (const QueueEntry &entry : Queue(m_folders.queue).entries())
            queued.insert(entry.run);
    QStringList active;
    for (const RunInfo &run : listRuns(m_folders, queued, now()))
        if (run.active || queued.contains(run.id))
            active.append(run.id);
    for (const QString &id : queued)
        if (!active.contains(id))
            active.append(id);
    active.sort();
    return active;
}

CacheStatus CacheManager::status(const CachePolicy &policy) const {
    CacheStatus status;
    QSet<QString> queued;
    if (!m_folders.queue.isEmpty())
        for (const QueueEntry &entry : Queue(m_folders.queue).entries())
            queued.insert(entry.run);

    if (!m_folders.results.isEmpty()) {
        const ResultCache cache(m_folders.results);
        KindUsage usage;
        usage.kind = CacheKind::Results;
        usage.bytes = cache.size();
        usage.files = cache.blobCount();
        usage.limit = policy.resultsBytes;
        QMap<QString, WorkflowUsage> workflows;
        QMap<QString, QMap<QString, NodeUsage>> nodes;
        for (const CacheEntry &entry : cache.entries()) {
            ++usage.items;
            usage.protectedItems += entry.frozen ? 1 : 0;
            if (!usage.oldest.isValid() || entry.lastUsed < usage.oldest)
                usage.oldest = entry.lastUsed;
            for (const UsedBy &by : entry.usedBy) {
                WorkflowUsage &workflow = workflows[by.workflow];
                workflow.workflow = by.workflow;
                workflow.bytes += entry.size;
                ++workflow.entries;
                NodeUsage &node = nodes[by.workflow][by.node];
                node.node = by.node;
                node.bytes += entry.size;
                ++node.entries;
            }
        }
        for (auto it = workflows.begin(); it != workflows.end(); ++it) {
            WorkflowUsage workflow = it.value();
            workflow.nodes = nodes.value(it.key()).values();
            std::sort(workflow.nodes.begin(), workflow.nodes.end(), [](const NodeUsage &a, const NodeUsage &b) {
                return a.bytes != b.bytes ? a.bytes > b.bytes : a.node < b.node;
            });
            status.workflows.append(workflow);
        }
        std::sort(status.workflows.begin(), status.workflows.end(), [](const WorkflowUsage &a, const WorkflowUsage &b) {
            return a.bytes != b.bytes ? a.bytes > b.bytes : a.workflow < b.workflow;
        });
        status.kinds.append(usage);
    }
    if (hasThumbnails(m_folders)) {
        KindUsage usage;
        usage.kind = CacheKind::Thumbnails;
        usage.limit = policy.thumbnailsBytes;
        for (const PictureFile &file : thumbnailFiles(m_folders)) {
            usage.bytes += file.bytes;
            ++usage.files;
            ++usage.items;
            if (!usage.oldest.isValid() || file.changed < usage.oldest)
                usage.oldest = file.changed;
        }
        status.kinds.append(usage);
    }
    if (!m_folders.runs.isEmpty()) {
        KindUsage usage;
        usage.kind = CacheKind::Runs;
        usage.limit = policy.runsKept;
        for (const RunInfo &run : listRuns(m_folders, queued, now())) {
            usage.bytes += run.bytes;
            usage.files += run.files;
            ++usage.items;
            usage.protectedItems += run.active ? 1 : 0;
            if (!usage.oldest.isValid() || run.when < usage.oldest)
                usage.oldest = run.when;
        }
        status.kinds.append(usage);
    }
    return status;
}

KindOutcome CacheManager::clearResults(const ClearRequest &request) const {
    KindOutcome outcome;
    outcome.kind = CacheKind::Results;
    ResultCache cache(m_folders.results);
    ResultCache::ClearFilter filter;
    filter.workflow = request.workflow;
    filter.node = request.node;
    if (request.olderThanSeconds > 0)
        filter.usedBefore = now().addSecs(-request.olderThanSeconds);
    for (const CacheEntry &entry : cache.entries()) {
        if (!entry.frozen)
            continue;
        const bool older = !filter.usedBefore.isValid() || entry.lastUsed < filter.usedBefore;
        bool uses = filter.workflow.isEmpty() && filter.node.isEmpty();
        for (const UsedBy &by : entry.usedBy)
            uses = uses || ((filter.workflow.isEmpty() || by.workflow == filter.workflow)
                            && (filter.node.isEmpty() || by.node == filter.node));
        outcome.kept += older && uses ? 1 : 0;
    }
    // A run takes the engine lock for as long as it goes; what it has stored must stay.
    Lock lock(m_folders.lock);
    if (!request.dryRun && !m_folders.lock.isEmpty() && !lock.tryAcquire()) {
        outcome.skipped = QStringLiteral("a run is going");
        return outcome;
    }
    const ResultCache::Cleared cleared = cache.clearMatching(filter, request.dryRun);
    outcome.removed = int(cleared.keys.size());
    outcome.bytes = cleared.bytes;
    return outcome;
}

KindOutcome CacheManager::clearThumbnails(const ClearRequest &request) const {
    KindOutcome outcome;
    outcome.kind = CacheKind::Thumbnails;
    const QDateTime cutoff = request.olderThanSeconds > 0 ? now().addSecs(-request.olderThanSeconds) : QDateTime();
    for (const PictureFile &file : thumbnailFiles(m_folders)) {
        if (cutoff.isValid() && !(file.changed < cutoff))
            continue;
        if (request.dryRun || QFile::remove(file.path)) {
            ++outcome.removed;
            outcome.bytes += file.bytes;
        }
    }
    return outcome;
}

KindOutcome CacheManager::clearRuns(const ClearRequest &request) const {
    KindOutcome outcome;
    outcome.kind = CacheKind::Runs;
    QSet<QString> queued;
    if (!m_folders.queue.isEmpty())
        for (const QueueEntry &entry : Queue(m_folders.queue).entries())
            queued.insert(entry.run);
    const QDateTime cutoff = request.olderThanSeconds > 0 ? now().addSecs(-request.olderThanSeconds) : QDateTime();
    for (const RunInfo &run : listRuns(m_folders, queued, now())) {
        if (!request.workflow.isEmpty() && run.workflow != request.workflow)
            continue;
        if (cutoff.isValid() && !(run.when < cutoff))
            continue;
        if (run.active || queued.contains(run.id)) {
            ++outcome.kept;
            continue;
        }
        if (request.dryRun || removeFolder(run.path)) {
            ++outcome.removed;
            outcome.bytes += run.bytes;
        }
    }
    return outcome;
}

Result<ClearReport> CacheManager::clear(const ClearRequest &request) const {
    QList<CacheKind> kinds = request.kinds.isEmpty() ? QList<CacheKind>{CacheKind::Results} : request.kinds;
    if (!request.node.isEmpty() && kinds != QList<CacheKind>{CacheKind::Results})
        return makeError(QStringLiteral("usage"), QStringLiteral("--node names a node of the result cache: use it with --kind results."));
    if (!request.workflow.isEmpty() && kinds.contains(CacheKind::Thumbnails))
        return makeError(QStringLiteral("usage"),
                         QStringLiteral("A preview picture belongs to no workflow: --workflow goes with --kind results or runs."));
    ClearReport report;
    report.dryRun = request.dryRun;
    for (const CacheKind kind : allCacheKinds()) {
        if (!kinds.contains(kind))
            continue;
        switch (kind) {
        case CacheKind::Results:
            if (!m_folders.results.isEmpty())
                report.kinds.append(clearResults(request));
            break;
        case CacheKind::Thumbnails:
            if (hasThumbnails(m_folders))
                report.kinds.append(clearThumbnails(request));
            break;
        case CacheKind::Runs:
            if (!m_folders.runs.isEmpty())
                report.kinds.append(clearRuns(request));
            break;
        }
    }
    return report;
}

ClearReport CacheManager::cleanupLocked(const CachePolicy &policy) const {
    ClearReport report;
    if (!m_folders.results.isEmpty()) {
        KindOutcome outcome;
        outcome.kind = CacheKind::Results;
        ResultCache cache(m_folders.results);
        cache.setLimit(policy.resultsBytes);
        const qint64 before = cache.size();
        outcome.removed = int(cache.evict().size());
        cache.collectGarbage();
        outcome.bytes = before - cache.size();
        report.kinds.append(outcome);
    }
    if (hasThumbnails(m_folders)) {
        KindOutcome outcome;
        outcome.kind = CacheKind::Thumbnails;
        const QDateTime cutoff = now().addDays(-CachePolicy::thumbnailDays);
        QList<PictureFile> files = thumbnailFiles(m_folders);
        qint64 total = 0;
        for (const PictureFile &file : files)
            total += file.bytes;
        for (const PictureFile &file : files) { // the least recently used first
            const bool stale = file.changed < cutoff;
            const bool over = policy.thumbnailsBytes > 0 && total > policy.thumbnailsBytes;
            if (!stale && !over)
                break;
            if (QFile::remove(file.path)) {
                total -= file.bytes;
                ++outcome.removed;
                outcome.bytes += file.bytes;
            }
        }
        report.kinds.append(outcome);
    }
    if (!m_folders.runs.isEmpty()) {
        KindOutcome outcome;
        outcome.kind = CacheKind::Runs;
        QSet<QString> queued;
        if (!m_folders.queue.isEmpty())
            for (const QueueEntry &entry : Queue(m_folders.queue).entries())
                queued.insert(entry.run);
        QList<RunInfo> ended;
        for (const RunInfo &run : listRuns(m_folders, queued, now()))
            if (!run.active && !queued.contains(run.id))
                ended.append(run);
        std::sort(ended.begin(), ended.end(), [](const RunInfo &a, const RunInfo &b) {
            return a.when != b.when ? a.when > b.when : a.id > b.id; // newest first
        });
        const QDateTime cutoff = now().addDays(-CachePolicy::runDays);
        for (int i = std::max(0, policy.runsKept); i < ended.size(); ++i) {
            if (!(ended.at(i).when < cutoff))
                continue;
            if (removeFolder(ended.at(i).path)) {
                ++outcome.removed;
                outcome.bytes += ended.at(i).bytes;
            }
        }
        report.kinds.append(outcome);
    }
    return report;
}

ClearReport CacheManager::cleanup(const CachePolicy &policy) const {
    Lock lock(m_folders.lock);
    if (!m_folders.lock.isEmpty() && !lock.tryAcquire()) {
        // A run holds the engine: leave its results and previews alone, and tidy the runs.
        CacheFolders runsOnly;
        runsOnly.runs = m_folders.runs;
        runsOnly.queue = m_folders.queue;
        ClearReport report = CacheManager(runsOnly, m_clock).cleanupLocked(policy);
        int at = 0;
        for (const CacheKind kind : {CacheKind::Results, CacheKind::Thumbnails}) {
            if (kind == CacheKind::Results ? m_folders.results.isEmpty() : !hasThumbnails(m_folders))
                continue;
            KindOutcome busy;
            busy.kind = kind;
            busy.skipped = QStringLiteral("a run is going");
            report.kinds.insert(at++, busy);
        }
        return report;
    }
    return cleanupLocked(policy);
}

} // namespace fugue
