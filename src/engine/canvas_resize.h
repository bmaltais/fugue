#pragma once

// Growing, shifting or cutting a canvas (#240, epic #231; docs/inpaint-outpaint-research.md gap G1): the
// pure half of CanvasStore::resize. A canvas is a PaintDocument; this gives the document that has the
// rectangle `to` of it, which may reach past the old canvas on any side (a new area) and may leave part
// of the old canvas out (cut).
//
// Every layer keeps its pixels where they were and is moved by the rectangle's corner, so the picture
// does not shift under the person: paint and mask layers are cut and re-placed (the new area is empty,
// transparent or black), a layer mask is cut the same way and is white (shows the layer) in a new area,
// and a picture layer stays a placeholder (the picture is a node's input, scaled to the canvas).
//
// When the canvas grows and `fill` is not Transparent, the new area of the new BOTTOM layer holds a
// pre-fill made from what the old canvas shows (the flattened visible layers; engine/edge_fill.h):
// the edge pixels stretched, and blurred for EdgeBlur. The rest of that layer is transparent, so the
// old picture lies over it unchanged and the pre-fill is one layer the person can hide or delete.

#include "engine/edge_fill.h"
#include "engine/paint_document.h"
#include "engine/result.h"

namespace fugue::mask {

inline constexpr const char *resizeFillLayerName = "Expanded area";
inline constexpr const char *resizeProducer = "canvas-resize";

// Whether `to` (in the old canvas's pixels) reaches past the old canvas on any side.
bool growsCanvas(int oldWidth, int oldHeight, const IRect &to);

// input_invalid for a size outside canvasSizeProblem's limit or a rectangle that holds none of the old canvas.
Result<PaintDocument> resizedCanvas(const PaintDocument &document, const IRect &to, edgefill::Mode fill,
                                    int blur = edgefill::defaultBlur);

} // namespace fugue::mask
