#include "result_cache.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimeZone>

#include <unistd.h>

#include "engine/canonical.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/json.h"

namespace fugue {

namespace {

const QLatin1String hashPrefix("sha256:");

QString hexOf(const QString &hash) {
    return hash.startsWith(hashPrefix) ? hash.mid(hashPrefix.size()) : hash;
}

QString isoTime(const QDateTime &time) {
    return time.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime parseTime(const QString &text) {
    QDateTime time = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (time.isValid())
        time = time.toUTC();
    return time;
}

bool isInside(const QString &path, const QString &dir) {
    return QDir::cleanPath(path).startsWith(QDir::cleanPath(dir) + QLatin1Char('/'));
}

bool hardLink(const QString &from, const QString &to) {
    return ::link(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
}

Error storeError(const QString &message) {
    return makeError(QStringLiteral("internal"), message);
}

} // namespace

// One key record. An output is tagged by what it is:
//   {"value": <json>}
//   {"media": <handle without path>, "blob": "<hex>.<ext>"}   a file in blobs/
//   {"media": <handle without path>, "path": "/abs/file"}     a file elsewhere
//   {"data": <handle without path>, "blob" | "path": ...}
//   {"list": [<tagged>, ...]}
struct ResultCache::Record {
    QString key;
    QString type;
    QJsonObject outputs;
    QMap<QString, qint64> blobs; // file name in blobs/ -> bytes
    QDateTime lastUsed;
    QList<UsedBy> usedBy;

    qint64 size() const {
        qint64 total = 0;
        for (qint64 bytes : blobs)
            total += bytes;
        return total;
    }

    QJsonObject toJson() const {
        QJsonObject sizes;
        for (auto it = blobs.begin(); it != blobs.end(); ++it)
            sizes.insert(it.key(), it.value());
        QJsonArray users;
        for (const UsedBy &by : usedBy)
            users.append(QJsonObject{{"workflow", by.workflow}, {"node", by.node}});
        return QJsonObject{{"key", key},         {"type", type},   {"outputs", outputs},
                           {"sizes", sizes},     {"size", size()}, {"last_used", isoTime(lastUsed)},
                           {"used_by", users}};
    }

    static Record fromJson(const QJsonObject &json) {
        Record record;
        record.key = json.value("key").toString();
        record.type = json.value("type").toString();
        record.outputs = json.value("outputs").toObject();
        const QJsonObject sizes = json.value("sizes").toObject();
        for (auto it = sizes.begin(); it != sizes.end(); ++it)
            record.blobs.insert(it.key(), qint64(it.value().toDouble()));
        record.lastUsed = parseTime(json.value("last_used").toString());
        for (const QJsonValue &user : json.value("used_by").toArray())
            record.usedBy.append(UsedBy{user.toObject().value("workflow").toString(),
                                        user.toObject().value("node").toString()});
        return record;
    }
};

QString cacheKey(const KeyParts &parts) {
    QJsonObject pack{{"id", parts.pack}};
    if (!parts.packVersion.isEmpty())
        pack.insert("version", parts.packVersion);
    QJsonObject what{{"type", parts.type},
                     {"version", parts.version.toString()},
                     {"pack", pack},
                     {"inputs", parts.inputs}};
    if (!parts.engine.isEmpty() || !parts.model.isEmpty())
        what.insert("provider", QJsonObject{{"engine", parts.engine}, {"model", parts.model}});
    if (!parts.run.isEmpty())
        what.insert("run", parts.run);
    return canonicalHash(what);
}

QString ResultCache::defaultRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/fugue/results");
}

ResultCache::ResultCache(QString root) : ResultCache(std::move(root), Options{}) {}

ResultCache::ResultCache(QString root, Options options) : m_root(std::move(root)), m_options(std::move(options)) {}

QString ResultCache::keyPath(const QString &key) const {
    return m_root + QStringLiteral("/keys/") + hexOf(key) + QStringLiteral(".json");
}

QString ResultCache::frozenPath(const QString &key) const {
    return m_root + QStringLiteral("/frozen/") + hexOf(key);
}

QString ResultCache::blobsDir() const { return m_root + QStringLiteral("/blobs"); }

QDateTime ResultCache::now() const {
    return m_options.clock ? m_options.clock().toUTC() : QDateTime::currentDateTimeUtc();
}

std::optional<ResultCache::Record> ResultCache::read(const QString &key) const {
    QFile file(keyPath(key));
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return std::nullopt;
    Record record = Record::fromJson(document.object());
    if (record.key.isEmpty())
        return std::nullopt;
    return record;
}

bool ResultCache::write(const Record &record) const {
    if (!QDir().mkpath(m_root + QStringLiteral("/keys")))
        return false;
    QSaveFile file(keyPath(record.key));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(writeJson(record.toJson()));
    return file.commit();
}

QList<ResultCache::Record> ResultCache::records() const {
    QList<Record> all;
    const QDir dir(m_root + QStringLiteral("/keys"));
    for (const QString &name : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        const std::optional<Record> record = read(hashPrefix + name.chopped(5));
        if (record)
            all.append(*record);
    }
    std::sort(all.begin(), all.end(), [](const Record &a, const Record &b) {
        return a.lastUsed != b.lastUsed ? a.lastUsed < b.lastUsed : a.key < b.key;
    });
    return all;
}

void ResultCache::removeRecord(const QString &key) const { QFile::remove(keyPath(key)); }

Result<Values> ResultCache::store(const QString &key, const QString &nodeType, const Values &outputs,
                                  const QString &nodeDir, const UsedBy &by, FileHashes &hashes) {
    if (!QDir().mkpath(blobsDir()))
        return storeError(QStringLiteral("Cannot create %1.").arg(blobsDir()));

    Record record;
    record.key = key;
    record.type = nodeType;
    record.lastUsed = now();
    record.usedBy.append(by);

    // Puts the file where it is to stay and fills in `tagged`; gives the
    // path the value passed on carries.
    auto place = [&](const QString &path, const QString &hash, QJsonObject &tagged) -> Result<QString> {
        if (path.isEmpty() || hash.isEmpty())
            return storeError(QStringLiteral("An output file has no path or no hash."));
        if (isInside(path, blobsDir())) {
            const QFileInfo info(path);
            record.blobs.insert(info.fileName(), info.size());
            tagged.insert("blob", info.fileName());
            return path;
        }
        if (!isInside(path, nodeDir)) {
            tagged.insert("path", path);
            return path;
        }
        const QString suffix = QFileInfo(path).suffix().toLower();
        const QString name = suffix.isEmpty() ? hexOf(hash) : hexOf(hash) + QLatin1Char('.') + suffix;
        const QString blob = blobsDir() + QLatin1Char('/') + name;
        if (QFile::exists(blob)) {
            if (!QFile::remove(path))
                return storeError(QStringLiteral("Cannot replace %1.").arg(path));
        } else {
            if (!QFile::rename(path, blob))
                return storeError(QStringLiteral("Cannot move %1 into the result cache.").arg(path));
            QFile::setPermissions(blob, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        }
        // The run folder keeps the file under its old name (9.7). On another
        // filesystem a hard link cannot be made, so a copy stands in.
        if (!hardLink(blob, path) && !QFile::copy(blob, path))
            return storeError(QStringLiteral("Cannot link %1 into the run folder.").arg(path));
        hashes.remember(blob, hash);
        record.blobs.insert(name, QFileInfo(blob).size());
        tagged.insert("blob", name);
        return blob;
    };

    std::function<Result<Value>(const Value &, QJsonObject &)> convert =
        [&](const Value &value, QJsonObject &tagged) -> Result<Value> {
        if (value.isMedia()) {
            const MediaHandle handle = value.media();
            const Result<QString> path = place(handle.path(), handle.hash(), tagged);
            if (!path)
                return path.error();
            const MediaHandle placed = handle.atPath(path.value());
            QJsonObject json = placed.toJson();
            json.remove("path");
            tagged.insert("media", json);
            return Value(placed);
        }
        if (value.isData()) {
            QJsonObject handle = value.json().toObject();
            const QString from = handle.value("path").toString();
            if (!handle.contains("hash"))
                handle.insert("hash", hashes.hash(from));
            const Result<QString> path = place(from, handle.value("hash").toString(), tagged);
            if (!path)
                return path.error();
            QJsonObject json = handle;
            json.remove("path");
            tagged.insert("data", json);
            handle.insert("path", path.value());
            return Value(QJsonValue(handle));
        }
        if (value.isList()) {
            QJsonArray list;
            QList<Value> items;
            for (const Value &item : value.items()) {
                QJsonObject taggedItem;
                const Result<Value> converted = convert(item, taggedItem);
                if (!converted)
                    return converted.error();
                list.append(taggedItem);
                items.append(converted.value());
            }
            tagged.insert("list", list);
            return Value::list(items);
        }
        tagged.insert("value", value.json());
        return value;
    };

    Values placed;
    for (auto it = outputs.begin(); it != outputs.end(); ++it) {
        if (it.value().isEmpty())
            continue;
        QJsonObject tagged;
        const Result<Value> converted = convert(it.value(), tagged);
        if (!converted)
            return converted.error();
        record.outputs.insert(it.key(), tagged);
        placed.insert(it.key(), converted.value());
    }
    if (!write(record))
        return storeError(QStringLiteral("Cannot write the cache record %1.").arg(keyPath(key)));
    return placed;
}

std::optional<Values> ResultCache::resolve(const Record &record, FileHashes &hashes) const {
    auto pathOf = [&](const QJsonObject &tagged, const QString &hash) -> QString {
        if (tagged.contains("blob")) {
            const QString path = blobsDir() + QLatin1Char('/') + tagged.value("blob").toString();
            return QFile::exists(path) ? path : QString();
        }
        // A file outside the store counts only while it has the same content.
        const QString path = tagged.value("path").toString();
        return !path.isEmpty() && QFile::exists(path) && hashes.hash(path) == hash ? path : QString();
    };

    std::function<std::optional<Value>(const QJsonObject &)> revive =
        [&](const QJsonObject &tagged) -> std::optional<Value> {
        if (tagged.contains("media")) {
            QJsonObject handle = tagged.value("media").toObject();
            const QString path = pathOf(tagged, handle.value("hash").toString());
            if (path.isEmpty())
                return std::nullopt;
            handle.insert("path", path);
            const Result<MediaHandle> media = MediaHandle::fromJson(handle);
            if (!media)
                return std::nullopt;
            return Value(media.value());
        }
        if (tagged.contains("data")) {
            QJsonObject handle = tagged.value("data").toObject();
            const QString path = pathOf(tagged, handle.value("hash").toString());
            if (path.isEmpty())
                return std::nullopt;
            handle.insert("path", path);
            return Value(QJsonValue(handle));
        }
        if (tagged.contains("list")) {
            QList<Value> items;
            for (const QJsonValue &item : tagged.value("list").toArray()) {
                const std::optional<Value> value = revive(item.toObject());
                if (!value)
                    return std::nullopt;
                items.append(*value);
            }
            return Value::list(items);
        }
        return Value(tagged.value("value"));
    };

    Values outputs;
    for (auto it = record.outputs.begin(); it != record.outputs.end(); ++it) {
        const std::optional<Value> value = revive(it.value().toObject());
        if (!value)
            return std::nullopt;
        outputs.insert(it.key(), *value);
    }
    return outputs;
}

std::optional<Values> ResultCache::peek(const QString &key, FileHashes &hashes) const {
    const std::optional<Record> record = read(key);
    if (!record)
        return std::nullopt;
    return resolve(*record, hashes);
}

std::optional<Values> ResultCache::lookup(const QString &key, const UsedBy &by, FileHashes &hashes) {
    std::optional<Record> record = read(key);
    if (!record)
        return std::nullopt;
    const std::optional<Values> outputs = resolve(*record, hashes);
    if (!outputs) {
        removeRecord(key);
        return std::nullopt;
    }
    record->lastUsed = now();
    if (!record->usedBy.contains(by))
        record->usedBy.append(by);
    write(*record);
    return outputs;
}

bool ResultCache::contains(const QString &key) const { return QFile::exists(keyPath(key)); }

bool ResultCache::freeze(const QString &key) {
    if (!contains(key) || !QDir().mkpath(m_root + QStringLiteral("/frozen")))
        return false;
    QFile marker(frozenPath(key));
    return marker.open(QIODevice::WriteOnly);
}

void ResultCache::unfreeze(const QString &key) { QFile::remove(frozenPath(key)); }

bool ResultCache::isFrozen(const QString &key) const { return QFile::exists(frozenPath(key)); }

QStringList ResultCache::frozen() const {
    QStringList keys;
    const QDir dir(m_root + QStringLiteral("/frozen"));
    for (const QString &name : dir.entryList(QDir::Files, QDir::Name))
        keys.append(hashPrefix + name);
    return keys;
}

QList<CacheEntry> ResultCache::entries() const {
    QList<CacheEntry> entries;
    for (const Record &record : records()) {
        CacheEntry entry;
        entry.key = record.key;
        entry.type = record.type;
        entry.size = record.size();
        entry.lastUsed = record.lastUsed;
        entry.usedBy = record.usedBy;
        entry.frozen = isFrozen(record.key);
        entries.append(entry);
    }
    return entries;
}

qint64 ResultCache::size() const {
    qint64 total = 0;
    for (const QFileInfo &info : QDir(blobsDir()).entryInfoList(QDir::Files))
        total += info.size();
    return total;
}

QStringList ResultCache::evict() {
    qint64 total = size();
    if (total <= m_options.limitBytes)
        return {};
    const QList<Record> all = records();
    QHash<QString, int> references;
    for (const Record &record : all)
        for (auto it = record.blobs.begin(); it != record.blobs.end(); ++it)
            ++references[it.key()];

    QStringList removed;
    for (const Record &record : all) {
        if (total <= m_options.limitBytes)
            break;
        if (isFrozen(record.key))
            continue;
        removeRecord(record.key);
        removed.append(record.key);
        for (auto it = record.blobs.begin(); it != record.blobs.end(); ++it) {
            if (--references[it.key()] > 0)
                continue;
            const QString blob = blobsDir() + QLatin1Char('/') + it.key();
            const qint64 bytes = QFileInfo(blob).size();
            if (QFile::remove(blob))
                total -= bytes;
        }
    }
    return removed;
}

QStringList ResultCache::clear(const QString &workflow, const QString &node) {
    QStringList removed;
    for (Record &record : records()) {
        if (isFrozen(record.key))
            continue;
        QList<UsedBy> kept;
        for (const UsedBy &by : record.usedBy) {
            const bool matches = (workflow.isEmpty() || by.workflow == workflow) && (node.isEmpty() || by.node == node);
            if (!matches)
                kept.append(by);
        }
        if (kept.isEmpty()) {
            removeRecord(record.key);
            removed.append(record.key);
        } else if (kept.size() != record.usedBy.size()) {
            record.usedBy = kept;
            write(record);
        }
    }
    // What was cleared goes at once; clearing is how a person frees space.
    if (!removed.isEmpty())
        removeUnreferencedBlobs(QDateTime());
    removed.sort();
    return removed;
}

void ResultCache::removeUnreferencedBlobs(const QDateTime &olderThan) const {
    QSet<QString> referenced;
    for (const Record &record : records())
        for (auto it = record.blobs.begin(); it != record.blobs.end(); ++it)
            referenced.insert(it.key());
    for (const QFileInfo &info : QDir(blobsDir()).entryInfoList(QDir::Files)) {
        if (referenced.contains(info.fileName()))
            continue;
        if (olderThan.isValid() && info.lastModified() >= olderThan)
            continue;
        QFile::remove(info.absoluteFilePath());
    }
}

void ResultCache::collectGarbage() {
    for (const Record &record : records()) {
        bool whole = true;
        std::function<void(const QJsonObject &)> check = [&](const QJsonObject &tagged) {
            if (tagged.contains("blob"))
                whole = whole && QFile::exists(blobsDir() + QLatin1Char('/') + tagged.value("blob").toString());
            else if (tagged.contains("path"))
                whole = whole && QFile::exists(tagged.value("path").toString());
            for (const QJsonValue &item : tagged.value("list").toArray())
                check(item.toObject());
        };
        for (auto it = record.outputs.begin(); it != record.outputs.end(); ++it)
            check(it.value().toObject());
        if (!whole)
            removeRecord(record.key);
    }
    removeUnreferencedBlobs(now().addSecs(-gracePeriodSeconds));
}

QStringList removeEndedRunFolders(const QString &runsRoot, const QDateTime &now, qint64 graceSeconds) {
    QStringList removed;
    const QDir root(runsRoot);
    for (const QString &id : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        QFile file(root.filePath(id) + QStringLiteral("/run.json"));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QDateTime ended = parseTime(QJsonDocument::fromJson(file.readAll()).object().value("ended").toString());
        if (!ended.isValid() || ended.addSecs(graceSeconds) >= now)
            continue;
        if (QDir(root.filePath(id)).removeRecursively())
            removed.append(id);
    }
    return removed;
}

} // namespace fugue
