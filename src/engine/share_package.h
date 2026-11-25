#pragma once

// A workflow packed to share (spec 11.5, #91): one .fuguepack file (a zip, with its own extension
// and MIME type, application/x-fugue-package, so the desktop opens it with Fugue) that another
// Fugue opens and runs. Opening tells one by its bytes, so a package renamed .zip opens too.
//
// The package is a template (11.10), zipped: `<id>/template.json` (the title), `<id>/workflow.fugue`
// (the share copy, engine/share_copy.h, holding the definition of every group it uses, a library's
// or a pack's included, and with `requires` refreshed so it names the packs it uses and where to
// get them), `<id>/inputs/` (each image a `file` input names that Fugue takes in,
// engine/image_format.h, the input then naming `inputs/<name>`) and `<id>/preview.<ext>` when a
// picture is given. Only such images go in, so a package never carries another file a node
// names (a key, a document); that file's input keeps its bare name, and the export says so. No
// model goes in: models stay references, by hash with their source and size, for the person
// to download (#89). So opening one is opening a template, and the person may keep it.
//
// An app bundle (#394, epic #397): the same zip with one reader, the extension `.fugueapp` and its own MIME type
// (application/x-fugue-app), for a workflow with an `app` block (engine/app_binding.h). It is the package above and two things
// more, so opening one needs nothing from the sender's machine but the models: `<id>/packs/<pack id>/` holds the exact folder of
// every pack the app's nodes use (the pack as installed, less its environment: dot files and folders, venv, __pycache__,
// node_modules, links; `requires.packs` records its source, ref and commit for updates), and `inputs/` holds, besides images,
// any file (a video, a song, a document) a node that an app input is bound to names, each at most 256 MB. Models stay
// references (hash, source, size). A bundle is only written when the receiver can get everything: a pack that can neither be
// carried nor fetched (no source), or a model with no download source, refuses the export (`app_not_shareable`, one error
// naming each item); a pack not carried but fetchable, or a sample file that is gone or too big, is a warning, the bundle
// written. A reader goes by content, so a `.fugueapp` renamed `.fuguepack` (or the reverse) opens the same.
//
// Unpacking takes only plain files and folders with relative names inside the package; an
// absolute name, a "..", a "\" or ":" (a Windows path), a link or a device is refused, and so
// is a file over 256 MB, a package over 4 GB unpacked or more than 10000 entries.

#include <QString>
#include <QStringList>

#include "engine/error.h"
#include "engine/result.h"
#include "engine/share_copy.h"
#include "engine/templates.h"

namespace fugue {

struct Document;
class Registry;

struct ShareRequest {
    QString title;        // the template's title: the workflow's tab
    QString workflowPath; // the file it was saved as, from which its relative inputs are found; may be empty
    QString preview;      // a picture it made (.png, .webp, .jpg), or empty
    ShareOptions options; // leave out the prompts, the layout
    QString appVersion;   // the `requires.fugue` it records
    // An app bundle (#394): carries the packs and every media file an app input names; refuses what the receiver cannot get.
    bool app = false;
    // Where installed packs are found, as packSearchPaths() has them; empty: the session's own (and the Default project's).
    QStringList packPaths;
    // Packs the template this workflow came from carries (`packs/<id>/`): looked in before the installed ones.
    QList<CarriedPack> carried;
};

// The file extension of a package: `fugueapp` for an app bundle, `fuguepack` for the rest.
inline constexpr const char *appPackageSuffix = "fugueapp";
inline constexpr const char *workflowPackageSuffix = "fuguepack";

// What `writeSharePackage` would refuse or warn about for an app bundle, without writing: errors `app_not_shareable` (refused: a
// pack with no folder here and no source, a model with no source) and warnings (a pack that is fetched, not carried; a sample
// file gone or too big). One error per item, named in its message.
Errors checkAppShare(const Document &document, const Registry &registry, const ShareRequest &request);

// Writes the package to `zipPath`, never over a file there; `included` gets the file names of
// the images put in. Errors: a file that cannot be read or written, images over 3 GB in all
// (`value_out_of_range`). Warnings, the package written: a file input whose file is not there,
// is not an image or a paint file, or is over 256 MB (its input keeps the bare name).
// `packsCarried` (an app bundle) gets the id of each pack put in.
Errors writeSharePackage(const Document &document, const Registry &registry, const ShareRequest &request,
                         const QString &zipPath, QStringList *included = nullptr, QStringList *packsCarried = nullptr);

// True when the file starts as a zip archive does.
bool isSharePackage(const QString &path);

// Unpacks the package at `zipPath` into `folder` (which must exist and be empty) and returns the
// folder it holds the template in: its one top folder, or, for a package whose files are at
// its top, a folder named after the zip that they are moved into. `invalid_document` for a
// package that is not one (no workflow.fugue, an entry refused).
Result<QString> unpackSharePackage(const QString &zipPath, const QString &folder);

} // namespace fugue
