#pragma once

// Remembered file hashes (spec 1.5, 1.4): a file's content hash is computed
// once and remembered by path, size and modification time, so a large model or
// image is not read again while it is unchanged.
//
// The store is one JSON file, ~/.local/state/fugue/hashes.json by default,
// shared by every Fugue process. save() merges with what is on disk and
// replaces it atomically, so processes that save at the same time lose no
// entries (an entry lost anyway only costs one more hash). A hash looked for and not
// remembered is looked for again in the store when another process has saved it since, so a
// store kept open for long (the window's run thread) sees a model the canvas hashed meanwhile.
//
// A project that inherits `models` (spec 1.4.2) also reads the Default's store, read-only, for the
// files under the inherited model roots, so a new project does not hash tens of GB again. The same
// validity rule applies (size and modification time must match), only entries of files under those
// roots are looked at, and the Default's file is never written: a hash the project makes or is
// told is saved to its own store, and what it took from the Default is not copied there.
//
// Thread-safe.

#include <QHash>
#include <QMutex>
#include <QString>

#include "engine/hash.h"

namespace fugue {

class FileHashes {
public:
    // A read-only second store: `storePath` is consulted, for a file under one of `roots` only.
    struct Inherited {
        QString storePath;
        QStringList roots;
    };

    // Loads `storePath` if it exists. A missing or damaged store is empty. Over defaultPath() it also
    // inherits what the session's project inherits (inheritedFromDefault()); any other store stands alone.
    explicit FileHashes(QString storePath);
    // The same with an explicit read-only store; no roots means none.
    FileHashes(QString storePath, Inherited inherited);
    FileHashes(const FileHashes &) = delete;
    FileHashes &operator=(const FileHashes &) = delete;

    // The Default project's store and the inherited model roots (the Default's models folder and
    // model_paths) when this session is a project that inherits `models`; empty otherwise (the Default
    // session, an anonymous project, one that does not list `models`: a strict project reads nothing).
    static Inherited inheritedFromDefault();

    // $XDG_STATE_HOME/fugue/hashes.json (~/.local/state/fugue/hashes.json).
    static QString defaultPath();

    // "sha256:<hex>" of the file at `path`: the remembered hash while the
    // file's size and modification time are unchanged, otherwise a fresh one,
    // which is then remembered. Empty when the file cannot be read.
    QString hash(const QString &path);
    // The same, telling `progress` as the file is read (sha256File); empty, and nothing
    // remembered, when it is stopped.
    QString hash(const QString &path, const HashProgress &progress);

    // The remembered hash of the file at `path` while its size and modification time are
    // unchanged; empty otherwise. Never reads the file.
    QString known(const QString &path);

    // For a file held open: its hash, with the identity (`size`, `mtimeNs`, from fstat of the open file)
    // taken from the open file, not from a path. The remembered hash of `shown` when those match; else
    // `readPath` (the open file's /proc link) is hashed and remembered under `shown` with them.
    QString hashOpen(const QString &shown, const QString &readPath, qint64 size, qint64 mtimeNs);

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

    struct StoreStamp {
        qint64 size = -1;
        qint64 mtimeNs = -1;
        bool operator==(const StoreStamp &other) const { return size == other.size && mtimeNs == other.mtimeNs; }
    };
    // The store file's entries, and its stamp when read.
    static QHash<QString, Entry> read(const QString &storePath, StoreStamp *stamp);
    static StoreStamp stampOf(const QString &storePath);
    // The remembered entry of `key` for a file of `size` and `mtimeNs`, looking in the store
    // again when it changed since it was last read. Call with m_mutex held.
    const Entry *lookup(const QString &key, qint64 size, qint64 mtimeNs);

    // The remembered entry of `key` in the inherited store, when it is under an inherited root and
    // for a file of `size` and `mtimeNs`. Call with m_mutex held.
    const Entry *inheritedLookup(const QString &key, qint64 size, qint64 mtimeNs);

    QString m_storePath;
    QString m_inheritedPath;
    QStringList m_inheritedRoots; // canonical, no trailing slash
    QHash<QString, Entry> m_inherited;
    StoreStamp m_inheritedStamp;
    QMutex m_mutex;
    QHash<QString, Entry> m_entries;
    StoreStamp m_stamp; // the store file as last read or written
};

} // namespace fugue
