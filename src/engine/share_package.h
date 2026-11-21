#pragma once

// A workflow packed to share (spec 11.5, #91): one .fuguepack file (a zip, with its own extension
// and MIME type, application/x-fugue-package, so the desktop opens it with Fugue) that another
// Fugue opens and runs. Opening tells one by its bytes, so a package renamed .zip opens too.
//
// The package is a template (11.10), zipped: `<id>/template.json` (the title), `<id>/workflow.fugue`
// (the share copy, engine/share_copy.h, with `requires` refreshed so it names the packs it uses
// and where to get them), `<id>/inputs/` (each image a `file` input names that Fugue takes in,
// engine/image_format.h, the input then naming `inputs/<name>`) and `<id>/preview.<ext>` when a
// picture is given. Only such images go in, so a package never carries another file a node
// names (a key, a document); that file's input keeps its bare name, and the export says so. No
// model goes in: models stay references, by hash with their source and size, for the person
// to download (#89). So opening one is opening a template, and the person may keep it.
//
// Unpacking takes only plain files and folders with relative names inside the package; an
// absolute name, a "..", a "\" or ":" (a Windows path), a link or a device is refused, and so
// is a file over 256 MB, a package over 4 GB unpacked or more than 10000 entries.

#include <QString>
#include <QStringList>

#include "engine/error.h"
#include "engine/result.h"
#include "engine/share_copy.h"

namespace fugue {

struct Document;
class Registry;

struct ShareRequest {
    QString title;        // the template's title: the workflow's tab
    QString workflowPath; // the file it was saved as, from which its relative inputs are found; may be empty
    QString preview;      // a picture it made (.png, .webp, .jpg), or empty
    ShareOptions options; // leave out the prompts, the layout
    QString appVersion;   // the `requires.fugue` it records
};

// Writes the package to `zipPath`, never over a file there; `included` gets the file names of
// the images put in. Errors: a file that cannot be read or written, images over 3 GB in all
// (`value_out_of_range`). Warnings, the package written: a file input whose file is not there,
// is not an image, or is over 256 MB (its input keeps the bare name).
Errors writeSharePackage(const Document &document, const Registry &registry, const ShareRequest &request,
                         const QString &zipPath, QStringList *included = nullptr);

// True when the file starts as a zip archive does.
bool isSharePackage(const QString &path);

// Unpacks the package at `zipPath` into `folder` (which must exist and be empty) and returns the
// folder it holds the template in: its one top folder, or, for a package whose files are at
// its top, a folder named after the zip that they are moved into. `invalid_document` for a
// package that is not one (no workflow.fugue, an entry refused).
Result<QString> unpackSharePackage(const QString &zipPath, const QString &folder);

} // namespace fugue
