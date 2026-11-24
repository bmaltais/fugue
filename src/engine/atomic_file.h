#pragma once

// Writing a file whole or not at all. The content goes to a hidden partial file beside the
// final name, which then takes that name in one step, so no one ever reads half a file.
// Every PartialFile has a name of its own, so two writers of one target (two Save Image
// nodes running at once) never share a partial file.
//
// Whether an existing file at the final name may be replaced is the caller's explicit
// choice: a store or node-folder file that is remade from the same input replaces, a file
// that may be the user's (Save Image) never does.

#include <QByteArray>
#include <QFile>
#include <QString>

#include "engine/result.h"

namespace fugue {

// The 13.3 code for a write that failed with `error` (an errno): file_not_found for a
// missing folder (ENOENT, ENOTDIR), permission_denied for a refusal (EACCES, EPERM, EROFS),
// node_failed for anything else (a full disk).
QString writeFailureCode(int error);

// Makes `dir` and the folders above it. Empty when it exists or was made; else one error
// with writeFailureCode's code, naming the folder and the cause.
Errors makeFolder(const QString &dir);

// A folder held open by its file descriptor. A file written through it lands in the folder
// that was opened, whatever is renamed or linked at its path afterwards (a link swapped in
// by another writer of a shared folder can no longer redirect the write). Every step down
// the path is opened with O_NOFOLLOW, so a link met on the way is refused, not followed.
class FolderFd {
public:
    // `dir`, an absolute path that is already canonical (no links, no `..`), opened from the root
    // folder by folder; missing folders are made when `create`. An error with writeFailureCode's
    // code naming the folder (permission_denied when a link is met).
    static Result<FolderFd> open(const QString &dir, bool create);
    // The folders of `relative` (a plain `a/b`, no `.`, `..` or empty part) below this one, opened
    // the same way. `create` makes the missing ones.
    Result<FolderFd> below(const QString &relative, bool create) const;

    FolderFd(FolderFd &&other) noexcept;
    FolderFd &operator=(FolderFd &&other) noexcept;
    FolderFd(const FolderFd &) = delete;
    FolderFd &operator=(const FolderFd &) = delete;
    ~FolderFd();

    int fd() const { return m_fd; }
    // The path it was opened by: for messages and for the paths handed back to people.
    const QString &path() const { return m_path; }

private:
    FolderFd(int fd, QString path) : m_fd(fd), m_path(std::move(path)) {}
    int m_fd = -1;
    QString m_path;
};

// Test seam: called, when set, after a path is judged fine and before FolderFd opens it, with
// that path, so a test can swap a folder for a link in that gap.
extern void (*beforeFolderOpen)(const QString &path);

enum class Overwrite {
    Replace, // an existing file at the name is replaced, in the same step
    Never,   // an existing file at the name is kept, even one made a moment ago by another writer
};

class PartialFile {
public:
    // A partial file for files in `dir` (which must exist) named after `name`:
    // `<dir>/.<name>.fugue-<pid>-<n>.part`, `name` shortened when needed to keep it within 255
    // bytes. Nothing is created until something writes it.
    PartialFile(const QString &dir, const QString &name);
    // The same in the open folder `dir` (which must outlive this): every step (create, write,
    // rename, remove) goes through its descriptor, never through a path.
    PartialFile(const FolderFd &dir, const QString &name);
    // Removes the partial file, unless it was committed.
    ~PartialFile();
    PartialFile(const PartialFile &) = delete;
    PartialFile &operator=(const PartialFile &) = delete;

    // The partial file's name in its folder (a PartialFile in a FolderFd).
    QString name() const { return QFile::decodeName(m_leaf); }

    // Where to write the content (a program such as ffmpeg may write it itself). In a FolderFd this is
    // /proc/<pid>/fd/<n> of the partial file held open (after create(); needs /proc and a process that
    // may be inspected, so not PR_SET_DUMPABLE 0), which no link at its name can redirect.
    QString path() const;
    // In a FolderFd: makes the partial file now (a program that writes path() needs it there; write()
    // does it itself). Empty when made; else an error. Nothing to do for a PartialFile in a path.
    Errors create() const;

    // Writes `bytes` as the whole content. On failure an error naming `what`, with
    // writeFailureCode's code.
    Errors write(const QByteArray &bytes, const QString &what) const;

    // Gives the written partial file the name `target` (in the same folder) in one step.
    // True when done. With Overwrite::Never and a file already at `target`, false: that file
    // is left as it is, and this partial file stays for another name. An error with
    // writeFailureCode's code when the move itself fails.
    Result<bool> commit(const QString &target, Overwrite overwrite);

    // Removes the partial files a writer left in `dir` when it died mid-write: names of this
    // class's form (`.<name>.fugue-<pid>-<n>.part`) whose process is gone, untouched for ten
    // minutes. A live writer's file, a fresh one (a writer on another machine sharing the
    // folder) and every other file stay.
    static void removeStale(const QString &dir);
    static void removeStale(const FolderFd &dir);

private:
    QString m_path;       // the whole path, or in a FolderFd the folder's path and the name, for messages
    const FolderFd *m_dir = nullptr;
    mutable int m_file = -1; // the partial file, open, in a FolderFd
    QByteArray m_leaf;    // the partial file's name in m_dir
    bool m_committed = false;
};

// Writes `bytes` as a new file at `path` through a PartialFile, never over a file there (an
// export, #91): permission_denied naming it when one is, or comes meanwhile.
Errors createFile(const QString &path, const QByteArray &bytes, const QString &what);

// Who may read a file replaceFile writes.
enum class FileMode {
    Usual,     // as the umask allows (0644 on most systems)
    OwnerOnly, // 0600 from the first byte: a file holding keys (providers.json, 8.2)
};

// Writes `bytes` as the whole of `path` through a PartialFile, replacing what is there in
// one step and making the folders above it. The content is on the disk (fsync) before it
// takes the name, so a power loss leaves the old file or the new one, never an empty one. A
// `path` that is a symbolic link stays one: the file it points to is replaced. Empty when
// done; else one error naming `what`.
Errors replaceFile(const QString &path, const QByteArray &bytes, const QString &what,
                   FileMode mode = FileMode::Usual);

} // namespace fugue
