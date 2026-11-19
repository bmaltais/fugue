#pragma once

// The folders a run's nodes may read and write files under (spec 13.6): a guard against
// mistakes and prompt injection, not a security boundary.
//
// `fugue run` sets them, from the folder it was started in, and the run folder carries
// them to the worker (roots.json, RunFolder::writeRoots). A run without them (the canvas,
// in-process tests) is not limited; the canvas decides for itself (#19).
//
// Paths are compared in canonical form, resolved one component at a time the way the
// kernel resolves them: a link is followed where it stands, so `link/..` is the folder above
// where the link leads, not the folder holding the link. Neither `..` nor a link leads out.
// The nodes then use the canonical path the check returns, so check and use agree.

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

#include "engine/result.h"

namespace fugue {

struct FileRoots {
    QStringList read;  // folders under which files may be read
    QStringList write; // folders under which files may be written

    // 13.6's defaults: read under the home folder; write under `workingDirectory` (where
    // `fugue run` was started), the output folder (paths.h) and ~/Documents/Fugue.
    // The inputs folder (paths.h inputsDir, under the output folder) is writable as part of it,
    // and readable always, as XDG_PICTURES_DIR may put it outside the home folder. With `workflowPath`
    // (the file a run was started from), the `inputs` folder beside it is readable too.
    static FileRoots defaults(const QString &workingDirectory, const QString &workflowPath = {});

    // The roots for storing an image: read and write the inputs folder, and nothing else.
    static FileRoots forInputs();

    bool allowsRead(const QString &path) const;
    bool allowsWrite(const QString &path) const;

    // The canonical path to read or write when `path` is inside, the one to use from then
    // on; else permission_denied on `input`, with a hint naming the roots.
    Result<QString> checkRead(const QString &path, const QString &input) const;
    Result<QString> checkWrite(const QString &path, const QString &input) const;

    QJsonObject toJson() const;
    static Result<FileRoots> fromJson(const QJsonObject &object);
};

// `path` in the form the roots compare: absolute, walked from `/` one component at a time.
// `.` is skipped, `..` drops the last component resolved so far, and a component that is a
// symbolic link is replaced by where it leads (followed again, 40 links at most). A
// component that does not exist yet is kept as written. Empty when the links loop.
QString canonicalPath(const QString &path);

// Whether canonical `path` is `root` or inside it (both canonical).
bool isInside(const QString &path, const QString &root);

} // namespace fugue
