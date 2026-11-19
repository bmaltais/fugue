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
#include <QSaveFile>
#include <QStandardPaths>

#include "engine/hash.h"
#include "engine/json.h"

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

FileHashes::FileHashes(QString storePath) : m_storePath(std::move(storePath)) {
    QFile file(m_storePath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value("fugue-hashes").toInt() != storeVersion)
        return;
    const QJsonObject files = root.value("files").toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        const QString hash = entry.value("hash").toString();
        if (isSha256(hash))
            m_entries.insert(it.key(), Entry{entry.value("size").toInteger(), entry.value("mtime_ns").toInteger(), hash});
    }
}

QString FileHashes::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) +
           QStringLiteral("/fugue/hashes.json");
}

QString FileHashes::hash(const QString &path) {
    const std::optional<Stat> stat = statFile(path);
    if (!stat)
        return {};
    const QString name = key(path);
    {
        QMutexLocker lock(&m_mutex);
        const auto it = m_entries.constFind(name);
        if (it != m_entries.constEnd() && it->size == stat->size && it->mtimeNs == stat->mtimeNs)
            return it->hash;
    }
    // Hashed outside the lock: a large file takes seconds.
    const QString hash = sha256File(path);
    const std::optional<Stat> after = statFile(path);
    if (hash.isEmpty() || !after || after->size != stat->size || after->mtimeNs != stat->mtimeNs)
        return hash; // changed while it was read: do not remember it
    QMutexLocker lock(&m_mutex);
    m_entries.insert(name, Entry{stat->size, stat->mtimeNs, hash});
    return hash;
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
    if (!lockFile.lock())
        return false;

    // What other processes saved, then ours on top, then only what is still true.
    FileHashes onDisk(m_storePath);
    QHash<QString, Entry> merged = onDisk.m_entries;
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it)
            merged.insert(it.key(), it.value());
    }
    QJsonObject files;
    for (auto it = merged.constBegin(); it != merged.constEnd(); ++it) {
        const std::optional<Stat> stat = statFile(it.key());
        if (!stat || stat->size != it->size || stat->mtimeNs != it->mtimeNs)
            continue;
        files.insert(it.key(), QJsonObject{{"size", it->size}, {"mtime_ns", it->mtimeNs}, {"hash", it->hash}});
    }

    QSaveFile file(m_storePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(writeJson(QJsonObject{{"fugue-hashes", storeVersion}, {"files", files}}));
    return file.commit();
}

} // namespace fugue
