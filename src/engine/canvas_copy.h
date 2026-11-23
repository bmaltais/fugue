#pragma once

// Copy and Copy Merged on the Canvas view (#327, epic #206; Photoshop's Edit > Copy, Copy Merged and Select > Save selection as a
// channel): the pure part, which knows no window, no clipboard and no store. The window reads the revision (CanvasStore::load), says
// what is selected and puts the answer on the clipboard.
//
// What is selected (CopyArea) is a painted selection (a one-channel picture of the canvas's size, 255 selected, a soft edge between),
// else a rectangle, else the whole canvas. A copy is cut to the bounds of what is selected and carries the selection as its alpha
// (the picture's own alpha times the selection), so a lasso copies a cut-out and a feathered edge fades out. The mask form is the
// selection itself at the canvas's size, white where it is selected, so it lines up with the canvas when used as a mask in nodes.

#include <optional>

#include "engine/mask_paint.h"
#include "engine/paint_document.h"
#include "engine/result.h"

namespace fugue::canvas {

struct CopyArea {
    // A painted selection: one channel, the canvas's size. Null: use `rect`.
    const mask::MaskPaint *selection = nullptr;
    // Canvas pixels; an empty rectangle with no selection is the whole canvas. Cut at the canvas's edge.
    mask::IRect rect;
};

struct CopiedPicture {
    mask::MaskPaint pixels{1, 1, 4}; // straight RGBA, the size of `bounds`
    mask::IRect bounds;              // where it was cut from, in canvas pixels
};

// A layer's own pixels at the canvas's size, its layer mask (when it has one switched on) multiplied into the alpha; its opacity and
// blend mode are not applied (as Copy in Photoshop, and as Export layers as files). input_invalid for a position that is not
// there or a mask layer (a selection, not a picture).
Result<mask::MaskPaint> layerPixels(const mask::PaintDocument &document, int position);
// The same for one layer of a document, by its info (what exportLayers reads).
mask::MaskPaint maskedPixels(const mask::PaintDocument &document, const mask::LayerInfo &layer, const mask::MaskPaint &pixels);

// Copy: `source` (a layer's pixels, or the flattened canvas for Copy Merged) cut to the area, alpha taken from the selection.
// input_invalid, in words, for a selection of another size, nothing selected, or a rectangle wholly off the canvas.
Result<CopiedPicture> copyArea(const mask::MaskPaint &source, const CopyArea &area);

// Copy selection as mask: one channel at the canvas's size, 255 where selected (a rectangle: inside it; no selection: all 255).
Result<mask::MaskPaint> areaAsMask(int width, int height, const CopyArea &area);

} // namespace fugue::canvas
