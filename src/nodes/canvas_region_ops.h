#pragma once

// The geometry of Canvas Region and Canvas Write (#209, epic #206, spec 14.5), on float pixel
// buffers and nothing else: no store, no node types, no libvips. Region reuses Crop to Mask's box
// and crop (crop_stitch_ops.h); Write reuses Stitch's feather. What is new here is the rectangle
// (instead of a mask), the size rule with a multiplier and a cap, and the layer Write places.

#include <optional>

#include "nodes/crop_stitch_ops.h"

namespace fugue::nodes {

// The rectangle x, y, width, height cut to a picture of canvasWidth x canvasHeight. A width or height
// of 0 runs to the picture's edge. None when nothing of it lies inside the picture.
std::optional<CropBox> rectBox(int x, int y, int width, int height, int canvasWidth, int canvasHeight);

// A one-channel mask of the picture's size, 1 inside `box`.
Pixels boxMask(const CropBox &box, int canvasWidth, int canvasHeight);

// The size a region is redrawn at (Krita's resolution multiplier and pixel cap): the box times
// `scale`, brought down to `capMegapixels` (0: no cap) keeping its shape, each side a multiple of
// `multiple` (nearest, one step at least; rounded down when the cap bites, so it is never passed)
// and none above kMaxCropSide.
CropSize regionSize(const CropBox &box, double scale, double capMegapixels, int multiple);

// The layer Canvas Write adds: RGBA, the picture's size, empty outside `box`. `redrawn` (any size)
// is resized to the box, and its alpha is `mask` (the picture's size) feathered outward by `blend`
// pixels, as Stitch blends, times the redrawn picture's own alpha. Where the alpha is 0 the color is
// 0 too, so an untouched tile costs nothing in the store.
Pixels placedLayer(int canvasWidth, int canvasHeight, const CropBox &box, const Pixels &redrawn, const Pixels &mask, int blend);

// The same placement kept apart as a layer and its layer mask (#226, Photoshop's Generative Fill):
// `layer` is the redrawn picture resized to the box with its own alpha (1 without one), whole, empty
// outside the box; `mask` is one channel, the picture's size, `mask` feathered outward by `blend`
// inside the box and 0 outside it. Their composite (alpha times mask) is placedLayer's within
// rounding, and the mask can later be turned off or painted to show more of the redrawn picture.
struct PlacedLayer {
    Pixels layer; // RGBA
    Pixels mask;  // one channel
};
PlacedLayer placedLayerWithMask(int canvasWidth, int canvasHeight, const CropBox &box, const Pixels &redrawn, const Pixels &mask, int blend);

} // namespace fugue::nodes
