#pragma once

// A canvas packed to share (#249, epic #206): a .fuguepack (the zip of engine/share_package.h, the same extension and
// MIME type) that carries a canvas of the revision store (engine/canvas_store.h) instead of a workflow. Opening one
// imports the canvas into the opener's store; it never replaces a canvas there.
//
// Layout: one folder `<name>/` holding
//   canvas.json          {"format": 1, "kind": "canvas", "id", "name", "mode": "head"|"history",
//                         "revisions": [{id, tree, parent, producer, time}, ...]}   oldest first, the head last
//   trees/<hex>.json     each revision's tree, as the store keeps it
//   tiles/<hex>.png      each tile of those trees once, named by the hash of its pixels
// `head` carries one revision, the head's layers with no history (its parent is cut, so it is a new first revision with
// its own id); `history` carries the head and every revision it came from (candidates, and what Undo left forward for
// Redo, stay out). Layer masks and every layer property travel; a picture layer is a placeholder, as in the store.
//
// The package is untrusted input. Unpacking uses engine/zip_archive.h (relative names inside the folder only, no links,
// sizes counted as they come); the files read are named by hashes the manifest gives after they pass isSha256, never by
// an entry's own name; a tree is parsed by the store's own reader and must hash to its name; a tile's PNG must say the
// size its place gives before it is decoded and must hold the pixels its name says; a revision must be the record its
// id names. The unpacking folder is made inside the store's root, so nothing is written outside it, and the canvas
// itself is written by CanvasStore::importCanvas, which checks all of it again and leaves nothing behind on failure.

#include <QString>

#include "engine/canvas_store.h"
#include "engine/error.h"
#include "engine/result.h"

namespace fugue::canvas {

enum class SharedPart { Head, History };

// Writes the package to `zipPath`, never over a file there. The canvas must have a head. Errors: canvas_not_found,
// canvas_corrupt (a stored object that is not what it is named for: nothing is written), a file that cannot be
// written, a package over 3 GB (value_out_of_range).
Errors writeCanvasPackage(const CanvasStore &store, const QString &canvasId, SharedPart part, const QString &zipPath);

// True when the file is a zip that holds a canvas.json in its top folder (the headers are read, nothing else).
bool isCanvasPackage(const QString &path);

struct CanvasImported {
    CanvasInfo info;     // the canvas as it is now in the store
    QString packagedId;  // the id it had when it was shared
    bool renamedId = false; // true: that id was taken here, so it has a new one
    SharedPart part = SharedPart::Head;
    int revisions = 0;
};

// Imports the canvas into `store` as a new canvas, named `name` or, when empty, as the package names it. A package
// that is not one is invalid_document; one that lies about a hash is canvas_corrupt.
Result<CanvasImported> importCanvasPackage(const QString &zipPath, CanvasStore &store, const QString &name = {});

} // namespace fugue::canvas
