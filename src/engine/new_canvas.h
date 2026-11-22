#pragma once

// The documents a new canvas starts from (#232, epic #206, spec 14.5): a blank one of a size and a
// background, or one from pixels (a clipboard picture). The window's New canvas dialog and
// `fugue canvas new` both make their first revision here, so the same size caps and the same words
// apply to both. Nothing here knows a window; a picture file is paint_file.h's.

#include <QString>

#include <cstdint>
#include <vector>

#include "engine/paint_document.h"
#include "engine/result.h"

namespace fugue::mask {

// What a blank canvas is filled with. Transparent leaves the first layer empty ("Layer 1"); any
// other fills it opaque, as Photoshop's Background layer ("Background").
struct CanvasBackground {
    bool filled = false;
    Rgba color;
};

// "transparent", "white", "black" or a hex color ("#rrggbb", "#rgb", with or without the #).
Result<CanvasBackground> parseBackground(const QString &text);

// Empty when a canvas may be this size, else the reason in plain words (at most paintFileMaxSide a
// side and paintFileMaxPixels in all).
QString canvasSizeProblem(int width, int height);
// "at most 16384 px a side and 67 megapixels", the cap as the dialog's line says it.
QString canvasSizeCap();

// One paint layer, filled as `background` says.
Result<PaintDocument> blankCanvas(int width, int height, const CanvasBackground &background);

// One paint layer of these straight RGBA pixels (row after row, 4 bytes each; a short buffer is padded
// with transparent), under the same caps.
Result<PaintDocument> canvasFromPixels(int width, int height, const std::vector<uint8_t> &rgba, const QString &layerName);

} // namespace fugue::mask
