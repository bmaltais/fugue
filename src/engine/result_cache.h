#pragma once

// The result cache on disk (spec 10): what each node produced, by cache key,
// so a node whose key is unchanged does not run again.
//
// Under the root (~/.cache/fugue/results/):
//   keys/<hex>.json     one record per key: the node type, its outputs (plain
//                       values, and media or data handles by content hash),
//                       sizes, the last-used time and `used_by`
//   blobs/<hex>.<ext>   the files, once per content hash, read-only
//   frozen/<hex>        a marker per frozen result (10.4)
// <hex> is the key or content hash without its "sha256:" prefix.
//
// A key is made by cacheKey() from the parts 10.1 lists; the scheduler gathers
// them. The cache itself never looks inside a key.
//
// Not thread-safe: use one ResultCache from one thread. Records are replaced
// atomically, so another process reading the store sees whole files.

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

#include "engine/clock.h"
#include "engine/result.h"
#include "engine/value.h"
#include "engine/version.h"

namespace fugue {

class FileHashes;

// The parts of a node's cache key (10.1).
struct KeyParts {
    QString type;    // the installed node type id
    Version version; // and its version
    QString pack;    // the pack id it came from
    QString packVersion; // empty until packs carry versions (M3)
    // Every input by name, as Value::identity(): a literal's value, the content
    // of an upstream output, a resource handle's identity hash. A file an
    // input names by path is {"file": <content hash>}.
    QJsonObject inputs;
    // Provider nodes only: the engine id and the identity of the model resolved.
    QString engine;
    QString model;
    // Only for a node type with `deterministic: false`: the run id.
    QString run;
};

// "sha256:<hex>" of the canonical JSON of the parts.
QString cacheKey(const KeyParts &parts);

// Who produced or reused a result: a workflow file and a node path id.
struct UsedBy {
    QString workflow; // absolute path, or empty for a graph run without a file
    QString node;

    bool operator==(const UsedBy &o) const { return workflow == o.workflow && node == o.node; }
};

// One record, for the cache views (`fugue cache status`, 10.3's size view).
struct CacheEntry {
    QString key;
    QString type;       // node type id
    qint64 size = 0;    // bytes of the blobs it refers to
    QDateTime lastUsed; // UTC
    QList<UsedBy> usedBy;
    bool frozen = false;
};

class ResultCache {
public:
    static constexpr qint64 defaultLimit = 20LL * 1024 * 1024 * 1024; // 20 GiB (10.5)
    static constexpr qint64 gracePeriodSeconds = 7 * 24 * 3600;       // 7 days (10.5)

    struct Options {
        qint64 limitBytes = defaultLimit;
        // The time source for last-used times and grace periods; the system
        // clock when unset. Tests pass a fake one.
        Clock clock;
    };

    // $XDG_CACHE_HOME/fugue/results (~/.cache/fugue/results).
    static QString defaultRoot();

    explicit ResultCache(QString root);
    ResultCache(QString root, Options options);

    QString root() const { return m_root; }

    // Records a finished node's outputs under `key` and returns them as they
    // are to be passed on. Each media or data file that lies inside `nodeDir`
    // is moved into blobs/ and a hard link is left in its place (9.7); the
    // value returned carries the store's path. A file anywhere else (a
    // user's file, a file a Save node wrote, a store blob passed through) is
    // left alone and recorded by path and hash. Media handles must have a
    // file and a hash already (MediaHandle::saved).
    Result<Values> store(const QString &key, const QString &nodeType, const Values &outputs,
                         const QString &nodeDir, const UsedBy &by, FileHashes &hashes);

    // The outputs recorded under `key`, or nothing. A record whose file is
    // gone or no longer has the recorded content is a miss, and is removed.
    // lookup() also marks the result used now, by `by`; peek() changes nothing.
    std::optional<Values> lookup(const QString &key, const UsedBy &by, FileHashes &hashes);
    std::optional<Values> peek(const QString &key, FileHashes &hashes) const;
    bool contains(const QString &key) const;

    // Freeze (10.4): a frozen result is never evicted or cleared. freeze()
    // fails when there is no record for the key.
    bool freeze(const QString &key);
    void unfreeze(const QString &key);
    bool isFrozen(const QString &key) const;
    QStringList frozen() const; // keys, sorted; includes markers whose record is gone

    // Every record, least recently used first.
    QList<CacheEntry> entries() const;
    // Bytes of all blobs.
    qint64 size() const;

    // Over the size limit, removes the least recently used records that are
    // not frozen, and the blobs nothing refers to any more, until the store
    // fits (10.5). Returns the keys removed.
    QStringList evict();

    // Clear all, one workflow's results, or one node's (10.5). A result
    // another workflow or node also uses only loses its `used_by` line.
    // Frozen results are kept. Returns the keys removed.
    QStringList clear(const QString &workflow = {}, const QString &node = {});

    // Garbage collection (10.5): removes key records whose files are gone,
    // and blobs no record refers to that are older than the grace period.
    void collectGarbage();

private:
    struct Record;
    QString keyPath(const QString &key) const;
    QString frozenPath(const QString &key) const;
    QString blobsDir() const;
    QDateTime now() const;
    std::optional<Record> read(const QString &key) const;
    bool write(const Record &record) const;
    QList<Record> records() const;
    void removeRecord(const QString &key) const;
    void removeUnreferencedBlobs(const QDateTime &olderThan) const;
    std::optional<Values> resolve(const Record &record, FileHashes &hashes) const;

    QString m_root;
    Options m_options;
};

// The other half of 10.5's garbage collection: removes the run folders under
// `runsRoot` whose run ended more than the grace period before `now`. A run
// that has not ended (no `ended` in run.json) is kept. Returns the ids removed.
QStringList removeEndedRunFolders(const QString &runsRoot, const QDateTime &now,
                                  qint64 graceSeconds = ResultCache::gracePeriodSeconds);

} // namespace fugue
