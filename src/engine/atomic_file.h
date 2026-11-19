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
    // Removes the partial file, unless it was committed.
    ~PartialFile();
    PartialFile(const PartialFile &) = delete;
    PartialFile &operator=(const PartialFile &) = delete;

    // Where to write the content (a program such as ffmpeg may write it itself).
    QString path() const { return m_path; }

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

private:
    QString m_path;
    bool m_committed = false;
};

// Who may read a file replaceFile writes.
enum class FileMode {
    Usual,     // as the umask allows (0644 on most systems)
    OwnerOnly, // 0600 from the first byte: a file holding keys (providers.json, 8.2)
};

// Writes `bytes` as the whole of `path` through a PartialFile, replacing what is there in
// one step and making the folders above it. Empty when done; else one error naming `what`.
Errors replaceFile(const QString &path, const QByteArray &bytes, const QString &what,
                   FileMode mode = FileMode::Usual);

} // namespace fugue
