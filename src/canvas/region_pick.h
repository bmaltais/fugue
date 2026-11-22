#pragma once

// The numbers behind the region picker (#213, epic #206, spec 14.5): what a rectangle on a canvas means
// for a Canvas Region node, on plain structs and nothing else (no Qt, no store, no picture). The
// rectangle's moves and resizes are engine/crop_geometry.h's, as the Crop window uses them; what is new
// is the snap to the node's `multiple`, and the two boxes the node derives from the rectangle, computed by the
// node's own functions (nodes/canvas_region_ops.h, nodes/crop_stitch_ops.h) so the dashed context box and
// the size shown are what the node will compute, not a copy of its rules.

#include "engine/crop_geometry.h"

namespace fugue::region {

// The inputs of Canvas Region that shape the context and the size.
struct Settings {
    int padding = 64;
    double scale = 1;
    double capMegapixels = 1.5;
    int multiple = 16;
};

// The rectangle the node takes from x, y, width, height on a canvas (a width or height of 0 runs to the
// edge; the rectangle is cut to the canvas): what the picker starts from. The whole canvas when the
// numbers leave nothing inside it, as an empty picker would be useless.
crop::Rect initial(int x, int y, int width, int height, const crop::Size &canvas);

// The rectangle with its edges on multiples of `multiple` (the nearest one; the canvas's own far edge
// counts as one, so the region can still reach it), at least one step wide and inside the canvas. With
// `keepSize` the size stays as it is and only the top left snaps (a ratio lock must not be broken by a
// snap). A multiple of 1 or less changes nothing.
crop::Rect snapped(const crop::Rect &rect, int multiple, const crop::Size &canvas, bool keepSize = false);

// The rectangle grown by the padding and kept inside the canvas: the picture the node cuts (the dashed box).
crop::Rect context(const crop::Rect &rect, int padding, const crop::Size &canvas);

// The size the node redraws the context at (scale, megapixel cap and multiple).
crop::Size modelSize(const crop::Rect &rect, const Settings &settings, const crop::Size &canvas);

// Whether the cap, not the scale, decided the size (the model size is smaller than scale times the context).
bool capped(const crop::Rect &rect, const Settings &settings, const crop::Size &canvas);

} // namespace fugue::region
