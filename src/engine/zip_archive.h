#pragma once

// Zip files for Fugue's packages (a workflow, engine/share_package.h; a canvas, engine/canvas_package.h): written
// from a list of entries, and unpacked with the checks an untrusted file needs. Nothing here knows what a package
// holds.
//
// Unpacking takes only plain files and folders with relative names inside the package: an absolute name, a "..",
// a "\" or ":" (a Windows path), a link or a device is refused, and so is a file, a total or a count over the limits
// the caller gives. Sizes are counted as the bytes come, whatever an entry says of its size. Dot files and
// __MACOSX (what other systems add) are skipped. A file the archive stores with the owner's execute bit is unpacked
// 0755 (the bit is read from the archive only; no other mode, never setuid, is restored). Nothing is ever written outside `folder`.

#include <QByteArray>
#include <QList>
#include <QString>

#include "engine/error.h"

namespace fugue {

// One file of a zip: its name in it, and its bytes or the file they are read from.
struct ZipEntry {
    QString name;
    QByteArray bytes;
    QString file;
    bool executable = false; // stored 0755 (a pack's run.sh), else 0644: only what the caller says is a program
};

// Writes `entries` as the zip `zip` (which is opened for writing, so the caller names it into place).
Errors writeZip(const QList<ZipEntry> &entries, const QString &zip);

// Writes `entries` as a zip named `zipPath`, never over a file there (permission_denied, "Pick another name"): written
// beside it, then named into place in one step.
Errors writeZipFile(const QList<ZipEntry> &entries, const QString &zipPath);

struct UnpackLimits {
    qint64 maxFile = 0;   // one file, unpacked
    qint64 maxTotal = 0;  // every file
    int maxEntries = 0;   // entries in the zip
};

// Unpacks `zipPath` into `folder` (which must exist). `invalid_document` for a zip that is not one or an entry
// that is refused, saying it is not a package; a write that fails is its own error.
Errors unpackZip(const QString &zipPath, const QString &folder, const UnpackLimits &limits);

// "<zip> is not a Fugue package: <why>."
Error notAPackage(const QString &zip, const QString &why);

} // namespace fugue
