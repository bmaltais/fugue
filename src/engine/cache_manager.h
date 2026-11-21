#pragma once

// What Fugue keeps under ~/.cache/fugue and how it is measured, cleared and kept in bounds
// (spec 10.6, #141). Three kinds, each a copy of something that can be made again:
//
//   results     the result cache (10): a missing result is a node that runs again
//   thumbnails  the 512 px pictures the canvas shows (12.7): a missing one is made again when
//               its result is shown
//   runs        the run folders (9.7): the history `fugue runs` reads
//
// Never touched: the output folder, the inputs folder, frozen results, and a run that is
// queued or going. Results and thumbnails are removed only while the engine lock is held
// (queue.h), which is how a run excludes everything else, so nothing is taken from under a
// run, and a run that waits for its turn starts after the cleaning ends.
//
// No Qt Quick: the CLI, the window's Cache page and the automatic cleanup all use this.
// Every call reads the disk afresh and keeps no state, so several managers may exist at once.

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

#include "engine/clock.h"
#include "engine/error.h"
#include "engine/result.h"

namespace fugue {

struct Settings;

enum class CacheKind { Results, Thumbnails, Runs };

// "results", "thumbnails", "runs".
QString cacheKindName(CacheKind kind);
std::optional<CacheKind> parseCacheKind(const QString &name);
QList<CacheKind> allCacheKinds();

// Where each kind lives. A kind with an empty folder is left out of every call.
struct CacheFolders {
    QString results;
    QString thumbnails;
    QString runs;
    QString queue; // the run queue folder (queue.h): runs in it are queued or going
    QString lock;  // the engine lock

    // The standard places: $XDG_CACHE_HOME/fugue/{results,thumbnails,runs} and the queue and
    // lock in the state folder.
    static CacheFolders standard();
};

// The limits (settings.h). A limit of 0 is none.
struct CachePolicy {
    static constexpr qint64 gigabyte = 1000LL * 1000 * 1000; // as sizeText() shows sizes
    static constexpr int thumbnailDays = 7;                  // a preview not shown for this long goes
    static constexpr int runDays = 7;                        // an ended run older than this goes (10.5)

    qint64 resultsBytes = 10 * gigabyte;
    qint64 thumbnailsBytes = gigabyte;
    int runsKept = 20; // the newest ended runs, kept whatever their age

    static CachePolicy from(const Settings &settings);
    static CachePolicy current(); // settings.json read afresh
};

// What one kind holds.
struct KindUsage {
    CacheKind kind = CacheKind::Results;
    qint64 bytes = 0;  // for runs: files no result refers to (a hard link into the store frees nothing)
    int files = 0;
    int items = 0;     // result records, picture files, run folders
    QDateTime oldest;  // last used (results), changed (thumbnails), ended or started (runs); invalid when empty
    qint64 limit = 0;  // the policy's, 0 for none (runs: the count kept)
    int protectedItems = 0; // frozen results, queued or going runs: never cleared

    QJsonObject toJson() const;
};

struct NodeUsage {
    QString node;
    qint64 bytes = 0;
    int entries = 0;
};
// A result is counted for each workflow and node that uses it, so these can add up to more than
// the results' total. `workflow` is empty for results made by a graph run with no file.
struct WorkflowUsage {
    QString workflow;
    qint64 bytes = 0;
    int entries = 0;
    QList<NodeUsage> nodes;
};

struct CacheStatus {
    QList<KindUsage> kinds; // the kinds of the folders given, in allCacheKinds() order
    QList<WorkflowUsage> workflows; // results by workflow, largest first
    const KindUsage *usage(CacheKind kind) const;
    qint64 totalBytes() const;
    QJsonObject toJson() const;
};

struct ClearRequest {
    QList<CacheKind> kinds;     // empty: results
    QString workflow;           // results and runs: the workflow file's absolute path
    QString node;               // results only: a node path id
    qint64 olderThanSeconds = 0; // 0: any age; the age is the last use (results) or end (runs)
    bool dryRun = false;
};

// What one kind lost, or would lose in a dry run.
struct KindOutcome {
    CacheKind kind = CacheKind::Results;
    int removed = 0;   // result records, picture files, run folders
    qint64 bytes = 0;  // freed
    int kept = 0;      // matching items that were left: frozen results, queued or going runs
    QString skipped;   // why nothing was done, when a run is going and holds the engine lock

    QJsonObject toJson() const;
};

struct ClearReport {
    bool dryRun = false;
    QList<KindOutcome> kinds;
    qint64 bytes() const;
    int removed() const;
    QJsonObject toJson() const;
};

class CacheManager {
public:
    explicit CacheManager(CacheFolders folders, Clock clock = {});

    const CacheFolders &folders() const { return m_folders; }

    // Sizes, counts and ages, and the results by workflow and node.
    CacheStatus status(const CachePolicy &policy = {}) const;

    // Clears what the request names. Errors: usage for a node with a kind that has no nodes,
    // a workflow with thumbnails.
    Result<ClearReport> clear(const ClearRequest &request) const;

    // The automatic cleaning, with the engine lock taken here (a run going skips it): results
    // over their limit lose the least recently used, previews not shown for a week and the
    // oldest over their limit go, ended runs older than a week go but the newest few. Never
    // frozen results, never a run that is queued or going.
    ClearReport cleanup(const CachePolicy &policy) const;
    // The same for a caller that holds the engine lock already (a run's own turn).
    ClearReport cleanupLocked(const CachePolicy &policy) const;

    // The runs queued or going: the queue's, and any folder not ended that is not abandoned.
    QStringList activeRuns() const;

private:
    QDateTime now() const { return utcNow(m_clock); }
    KindOutcome clearResults(const ClearRequest &request) const;
    KindOutcome clearThumbnails(const ClearRequest &request) const;
    KindOutcome clearRuns(const ClearRequest &request) const;

    CacheFolders m_folders;
    Clock m_clock;
};

} // namespace fugue
