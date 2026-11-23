#include "file_hashes.h"

#include <sys/stat.h>

#include <optional>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutexLocker>

#include "engine/hash.h"
#include "engine/json.h"
#include "engine/model_library.h"
#include "engine/paths.h"
#include "engine/settings.h"

namespace fugue {

namespace {

constexpr int storeVersion = 1;

struct Stat {
    qint64 size = 0;
    qint64 mtimeNs = 0;
};

// Size and modification time to the nanosecond; QFileInfo only gives
// milliseconds, which would let a same-size rewrite keep a stale hash.
std::optional<Stat> statFile(const QString &path) {
    struct stat info;
    if (::stat(QFile::encodeName(path).constData(), &info) != 0 || !S_ISREG(info.st_mode))
        return std::nullopt;
    return Stat{qint64(info.st_size), qint64(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec};
}

QString key(const QString &path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

} // namespace

FileHashes::FileHashes(QString storePath) : FileHashes(storePath, storePath == defaultPath() ? inheritedFromDefault() : Inherited()) {}

FileHashes::FileHashes(QString storePath, Inherited inherited) : m_storePath(std::move(storePath)) {
    m_entries = read(m_storePath, &m_stamp);
    for (const QString &root : inherited.roots) {
        const QString clean = key(root);
        if (!clean.isEmpty() && clean != QLatin1String("/") && !m_inheritedRoots.contains(clean))
            m_inheritedRoots << clean;
    }
    // Never the project's own store (a project at the Default's place would read itself).
    if (!m_inheritedRoots.isEmpty() && key(inherited.storePath) != key(m_storePath)) {
        m_inheritedPath = inherited.storePath;
        m_inherited = read(m_inheritedPath, &m_inheritedStamp);
    } else {
        m_inheritedRoots.clear();
    }
}

FileHashes::Inherited FileHashes::inheritedFromDefault() {
    const Settings settings = currentSettings();
    if (!settings.inheritsModels)
        return {};
    Inherited out;
    out.storePath = baseStateDir() + QStringLiteral("/hashes.json");
    for (const SearchPath &root : ModelLibrary::taggedRoots(settings))
        if (root.inherited)
            out.roots << root.path;
    return out;
}

const FileHashes::Entry *FileHashes::inheritedLookup(const QString &key, qint64 size, qint64 mtimeNs) {
    if (m_inheritedRoots.isEmpty())
        return nullptr;
    bool under = false;
    for (const QString &root : std::as_const(m_inheritedRoots))
        if (key.startsWith(root + QLatin1Char('/'))) {
            under = true;
            break;
        }
    if (!under)
        return nullptr;
    const auto matches = [&](const Entry &entry) { return entry.size == size && entry.mtimeNs == mtimeNs; };
    if (const auto it = m_inherited.constFind(key); it != m_inherited.constEnd() && matches(*it))
        return &*it;
    // The Default saved since (the window outlives a Default session's run): read it again.
    if (stampOf(m_inheritedPath) == m_inheritedStamp)
        return nullptr;
    m_inherited = read(m_inheritedPath, &m_inheritedStamp);
    const auto it = m_inherited.constFind(key);
    return it != m_inherited.constEnd() && matches(*it) ? &*it : nullptr;
}

FileHashes::StoreStamp FileHashes::stampOf(const QString &storePath) {
    const std::optional<Stat> stat = statFile(storePath);
    return stat ? StoreStamp{stat->size, stat->mtimeNs} : StoreStamp{};
}

QHash<QString, FileHashes::Entry> FileHashes::read(const QString &storePath, StoreStamp *stamp) {
    if (stamp)
        *stamp = stampOf(storePath);
    // A memo: one that is missing, corrupt or of another version is simply started again.
    const Result<QJsonObject> json = readJsonObject(storePath);
    const QJsonObject root = json ? json.value() : QJsonObject();
    QHash<QString, Entry> entries;
    if (root.value("fugue-hashes").toInt() != storeVersion)
        return entries;
    const QJsonObject files = root.value("files").toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        const QString hash = entry.value("hash").toString();
        if (isSha256(hash))
            entries.insert(it.key(), Entry{entry.value("size").toInteger(), entry.value("mtime_ns").toInteger(), hash});
    }
    return entries;
}

const FileHashes::Entry *FileHashes::lookup(const QString &key, qint64 size, qint64 mtimeNs) {
    const auto matches = [&](const Entry &entry) { return entry.size == size && entry.mtimeNs == mtimeNs; };
    if (const auto it = m_entries.constFind(key); it != m_entries.constEnd() && matches(*it))
        return &*it;
    if (stampOf(m_storePath) == m_stamp)
        return inheritedLookup(key, size, mtimeNs);
    // Another process saved since: take what it knows that this one does not.
    const QHash<QString, Entry> stored = read(m_storePath, &m_stamp);
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        const auto ours = m_entries.constFind(it.key());
        if (ours == m_entries.constEnd() || ours->size != it->size || ours->mtimeNs != it->mtimeNs)
            m_entries.insert(it.key(), it.value());
    }
    const auto it = m_entries.constFind(key);
    if (it != m_entries.constEnd() && matches(*it))
        return &*it;
    return inheritedLookup(key, size, mtimeNs);
}

QString FileHashes::defaultPath() {
    return stateDir() + QStringLiteral("/hashes.json");
}

QString FileHashes::hash(const QString &path) { return hash(path, {}); }

QString FileHashes::hash(const QString &path, const HashProgress &progress) {
    const std::optional<Stat> stat = statFile(path);
    if (!stat)
        return {};
    const QString name = key(path);
    {
        QMutexLocker lock(&m_mutex);
        if (const Entry *entry = lookup(name, stat->size, stat->mtimeNs))
            return entry->hash;
    }
    // Hashed outside the lock: a large file takes seconds.
    const QString hash = sha256File(path, progress);
    const std::optional<Stat> after = statFile(path);
    if (hash.isEmpty() || !after || after->size != stat->size || after->mtimeNs != stat->mtimeNs)
        return hash; // changed while it was read: do not remember it
    QMutexLocker lock(&m_mutex);
    m_entries.insert(name, Entry{stat->size, stat->mtimeNs, hash});
    return hash;
}

QString FileHashes::known(const QString &path) {
    const std::optional<Stat> stat = statFile(path);
    if (!stat)
        return {};
    QMutexLocker lock(&m_mutex);
    const Entry *entry = lookup(key(path), stat->size, stat->mtimeNs);
    return entry ? entry->hash : QString();
}

void FileHashes::remember(const QString &path, const QString &hash) {
    const std::optional<Stat> stat = statFile(path);
    if (!stat || !isSha256(hash))
        return;
    QMutexLocker lock(&m_mutex);
    m_entries.insert(key(path), Entry{stat->size, stat->mtimeNs, hash});
}

bool FileHashes::save() {
    QDir().mkpath(QFileInfo(m_storePath).absolutePath());
    QLockFile lockFile(m_storePath + QStringLiteral(".lock"));
    lockFile.setStaleLockTime(30000);
    // A deadline, never forever: a holder that is alive but stuck must not hang every other process (#205).
    if (!lockFile.tryLock(10000)) {
        qWarning("FileHashes: %s is held by another process; not saved", qPrintable(m_storePath + QStringLiteral(".lock")));
        return false;
    }

    // What other processes saved, then ours on top, then only what is still true. The files
    // are looked at and the store written without the lock, so a lookup meanwhile does not wait.
    QHash<QString, Entry> ours;
    {
        QMutexLocker lock(&m_mutex);
        ours = m_entries;
    }
    QHash<QString, Entry> merged = read(m_storePath, nullptr);
    for (auto it = ours.constBegin(); it != ours.constEnd(); ++it) {
        // One of ours the file has moved past does not replace another process's newer entry.
        const std::optional<Stat> stat = statFile(it.key());
        if (merged.contains(it.key()) && (!stat || stat->size != it->size || stat->mtimeNs != it->mtimeNs))
            continue;
        merged.insert(it.key(), it.value());
    }
    QJsonObject files;
    for (auto it = merged.begin(); it != merged.end();) {
        const std::optional<Stat> stat = statFile(it.key());
        if (!stat || stat->size != it->size || stat->mtimeNs != it->mtimeNs) {
            it = merged.erase(it);
            continue;
        }
        files.insert(it.key(), QJsonObject{{"size", it->size}, {"mtime_ns", it->mtimeNs}, {"hash", it->hash}});
        ++it;
    }

    if (!writeJsonFile(m_storePath, QJsonObject{{"fugue-hashes", storeVersion}, {"files", files}}).isEmpty())
        return false;
    // The store now holds what this one knew and what the others saved: keep the same, with
    // anything remembered while it was written.
    QMutexLocker lock(&m_mutex);
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it)
        if (!ours.contains(it.key()) || ours.value(it.key()).hash != it->hash)
            merged.insert(it.key(), it.value());
    m_entries = std::move(merged);
    m_stamp = stampOf(m_storePath);
    return true;
}

} // namespace fugue
