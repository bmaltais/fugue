#pragma once

// Remembered file hashes (spec 1.5, 1.4): a file's content hash is computed
// once and remembered by path, size and modification time, so a large model or
// image is not read again while it is unchanged.
//
// The store is one JSON file, ~/.local/state/fugue/hashes.json by default,
// shared by every Fugue process. save() merges with what is on disk and
// replaces it atomically, so processes that save at the same time lose no
// entries (an entry lost anyway only costs one more hash).
//
// Thread-safe.

#include <QHash>
#include <QMutex>
#include <QString>

namespace fugue {

class FileHashes {
public:
    // Loads `storePath` if it exists. A missing or damaged store is empty.
    explicit FileHashes(QString storePath);
    FileHashes(const FileHashes &) = delete;
    FileHashes &operator=(const FileHashes &) = delete;

    // $XDG_STATE_HOME/fugue/hashes.json (~/.local/state/fugue/hashes.json).
    static QString defaultPath();

    // "sha256:<hex>" of the file at `path`: the remembered hash while the
    // file's size and modification time are unchanged, otherwise a fresh one,
    // which is then remembered. Empty when the file cannot be read.
    QString hash(const QString &path);

    // The remembered hash of the file at `path` while its size and modification time are
    // unchanged; empty otherwise. Never reads the file.
    QString known(const QString &path);

    // Records that the file now at `path` has `hash`, without reading it. For
    // a file whose bytes are known, such as one just moved or hard-linked into
    // the result cache's store (a rename keeps size and modification time).
    void remember(const QString &path, const QString &hash);

    // Writes the store. Entries whose file is gone or has changed are dropped.
    bool save();

private:
    struct Entry {
        qint64 size = 0;
        qint64 mtimeNs = 0;
        QString hash;
    };

    QString m_storePath;
    QMutex m_mutex;
    QHash<QString, Entry> m_entries;
};

} // namespace fugue
