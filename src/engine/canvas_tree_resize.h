#pragma once

// Growing, shifting or cutting a canvas at the tree level (#274, spec 14.5): what CanvasStore::resize stores.
// The same result as engine/canvas_resize.h gives a PaintDocument (every layer keeps its pixels where they were and
// moves by the rectangle's corner; the new area is transparent, black, or, for the optional new bottom layer, a
// pre-fill), but on the layer records, so a group stays a group: its own layer mask is moved like any layer's and
// every child is resized the same way, however deep. A tree without groups gives the tree the document path gave
// (the same tiles, so the same hash). Pure otherwise: the tiles are read and written through two functions.

#include "engine/canvas_layer_merge.h"
#include "engine/canvas_resize.h"

namespace fugue::canvas {

// input_invalid for a size outside canvasSizeProblem's limit or a rectangle that holds none of the old canvas.
Result<Tree> resizedTree(const Tree &tree, const mask::IRect &to, edgefill::Mode fill, int blur, const TileReader &read, const TileSink &consume);

} // namespace fugue::canvas
