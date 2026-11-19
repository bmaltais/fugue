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

enum class Overwrite {
    Replace, // an existing file at the name is replaced, in the same step
    Never,   // an existing file at the name is kept, even one made a moment ago by another writer
};

class PartialFile {
public:
    // A partial file for files in `dir` (which must exist) named after `name`:
    // `<dir>/.<name>.<unique>.part`, `name` shortened when needed to keep it within 255
    // bytes. Nothing is created until something writes it.
    PartialFile(const QString &dir, const QString &name);
    // Removes the partial file, unless it was committed.
    ~PartialFile();
    PartialFile(const PartialFile &) = delete;
    PartialFile &operator=(const PartialFile &) = delete;

    // Where to write the content (a program such as ffmpeg may write it itself).
    QString path() const { return m_path; }

    // Writes `bytes` as the whole content. permission_denied naming `what` on failure.
    Errors write(const QByteArray &bytes, const QString &what) const;

    // Gives the written partial file the name `target` (in the same folder) in one step.
    // True when done. With Overwrite::Never and a file already at `target`, false: that file
    // is left as it is, and this partial file stays for another name. permission_denied when
    // the move itself fails.
    Result<bool> commit(const QString &target, Overwrite overwrite);

private:
    QString m_path;
    bool m_committed = false;
};

} // namespace fugue
