#pragma once

// A paint canvas as one file (#181, spec 14.4): a PaintDocument as an OpenRaster (.ora) zip, the
// file a `paint` input names in the inputs folder (a text literal, as `mask` holds a gray PNG).
//
//   mimetype        "image/openraster", first and stored (the format's own rule)
//   stack.xml       the canvas size and each layer: name, visibility, opacity, kind (fugue:kind)
//   data/layer-N.png  one PNG per layer, MaskPaint::toPng (RGBA for paint, gray for mask)
//
// The same layers always give the same bytes: layers in one fixed order (bottom first by
// position, N is the position), every entry stored (PNG is compressed already), every time and
// owner fixed, and nothing that varies (no thumbnail, no merged image, no program or date).
//
// A picture layer is a placeholder: the picture it stands for is the node's input, never part of
// the file (so the file does not hold, or change with, the picture), and its PNG is one
// transparent pixel. Any other OpenRaster reader sees a layer that is empty.
//
// Reading is for hostile files: a size cap on the canvas, the layers, each entry and the whole,
// the names checked, the PNG's own size read before it is decoded, and a file that is not exactly
// what Fugue writes is refused with media_unreadable (no guessing at other programs' offsets,
// groups or blend modes). Nothing is ever written to disk by reading.

#include <QByteArray>
#include <QString>

#include "engine/paint_document.h"
#include "engine/result.h"

namespace fugue::mask {

inline constexpr int paintFileMaxSide = 16384;
inline constexpr qint64 paintFileMaxPixels = qint64(8192) * 8192; // a canvas
inline constexpr int paintFileMaxLayers = 64;
inline constexpr qint64 paintFileMaxLayerBytes = qint64(1) << 30; // every layer's pixels together, once decoded
inline constexpr qint64 paintFileMaxBytes = qint64(1) << 30;      // the file itself

// The document as the bytes of an .ora file. The undo history and the active layer are not saved.
Result<QByteArray> paintToOra(const PaintDocument &document);

// An .ora file's bytes as a document (no history; the top layer active), or media_unreadable.
Result<PaintDocument> paintFromOra(const QByteArray &bytes);

// Whether the bytes begin as a file Fugue writes (a zip whose first entry is the mimetype): a cheap
// test for the share package and templates, which then read the whole with paintFromOra.
bool looksLikeOra(QByteArrayView bytes);

// A PNG, JPEG or WebP picture as a one-layer document (a paint layer named `layerName`, the picture's
// size, no history), or media_unreadable / value_out_of_range.
bool looksLikePicture(QByteArrayView bytes);
Result<PaintDocument> paintFromPicture(const QByteArray &bytes, const QString &layerName);
// A file a canvas can be made from: an .ora (paintFromOra) or a picture (paintFromPicture, named after
// the file). Anything else is media_unreadable and names the supported kinds.
Result<PaintDocument> readCanvasFile(const QString &path);

// Writes the document to `path` (replacing a file there, in one step) and reads one back.
Errors writePaintFile(const PaintDocument &document, const QString &path);
Result<PaintDocument> readPaintFile(const QString &path);

} // namespace fugue::mask
