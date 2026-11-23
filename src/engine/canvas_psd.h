#pragma once

// Photoshop files and a canvas store (#227, epic #206): what psd_file.h cannot do alone because a canvas has groups
// (#252) and a PaintDocument has not. A revision's tree goes out with its groups as PSD layer groups; a PSD comes in with
// the groups Fugue can keep as real groups (the others are flattened with a warning, psd_file.h). Nothing here knows a window.

#include <QByteArray>
#include <QString>

#include "engine/canvas_export.h"
#include "engine/canvas_store.h"
#include "engine/psd_file.h"
#include "engine/result.h"

namespace fugue::canvas {

// The tree as a PSD: its groups are layer groups, a mask layer (a selection) is left out and named in the notes, the merged
// image is the tree as drawn. `read` gives a stored tile (CanvasStore::tilePixels).
Result<mask::PsdExported> psdOfTree(const Tree &tree, const std::function<Result<mask::MaskPaint>(const QString &hash, int width, int height, int channels)> &read);

// A revision as `options.format` psd. A canvas with groups keeps them; a size other than the canvas's flattens the groups
// (the layers are scaled as documents are), as exportPicture does for every format.
Result<ExportedPicture> exportRevisionAsPsd(const CanvasStore &store, const QString &canvasId, const QString &revisionId, const PictureExport &options);

// A new canvas from a .psd's bytes, in ONE revision (producer `producer`): real groups where Fugue can keep them. The
// Result's warnings list what was dropped or flattened.
Result<CanvasInfo> importPsd(CanvasStore &store, const QString &name, const QByteArray &bytes, const QString &producer, int *width = nullptr, int *height = nullptr);

// A new canvas from any file a canvas can be made from: a .psd as importPsd, anything else as readCanvasFile then create.
// The warnings of either come with the Result.
Result<CanvasInfo> createCanvasFromFile(CanvasStore &store, const QString &name, const QString &path, const QString &producer, int *width = nullptr, int *height = nullptr);

} // namespace fugue::canvas
