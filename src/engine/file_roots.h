#pragma once

// The folders a run's nodes may read and write files under (spec 13.6): a guard against
// mistakes and prompt injection, not a security boundary.
//
// `fugue run` sets them, from the folder it was started in, and the run folder carries
// them to the worker (roots.json, RunFolder::writeRoots). A run without them (the canvas,
// in-process tests) is not limited; the canvas decides for itself (#19).
//
// Paths are compared in canonical form: made absolute, `.` and `..` resolved, and every
// symbolic link of the part that exists resolved, so neither `..` nor a link leads out.

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
    static FileRoots defaults(const QString &workingDirectory);

    bool allowsRead(const QString &path) const;
    bool allowsWrite(const QString &path) const;

    // permission_denied on `input` when the path is outside, with a hint naming the roots;
    // empty when it is inside.
    Errors checkRead(const QString &path, const QString &input) const;
    Errors checkWrite(const QString &path, const QString &input) const;

    QJsonObject toJson() const;
    static Result<FileRoots> fromJson(const QJsonObject &object);
};

// `path` in the form the roots compare: absolute, cleaned, and with the existing part's
// symbolic links resolved (the rest, not made yet, is appended as written).
QString canonicalPath(const QString &path);

// Whether canonical `path` is `root` or inside it (both canonical).
bool isInside(const QString &path, const QString &root);

} // namespace fugue
