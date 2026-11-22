#pragma once

// The arithmetic of Crop to Mask and Stitch (spec 14.4), on float pixel buffers and nothing else:
// no libvips, no node types. The nodes in crop_stitch.cpp call these.

#include <optional>

#include "nodes/mask_ops.h"
#include "nodes/resample.h"

namespace fugue::nodes {

// A rectangle of whole pixels: its top left, then its size.
struct CropBox {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const CropBox &o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
};

struct CropSize {
    int width = 0;
    int height = 0;
    bool operator==(const CropSize &o) const { return width == o.width && height == o.height; }
};

// The smallest box holding every pixel of `mask` above 0 (its first channel); none for a mask with
// nothing in it.
std::optional<CropBox> maskBounds(const Pixels &mask);

// `bounds` grown by `padding` on each side, then moved inside the picture where it sticks out and
// cut where the picture is smaller than the grown box. A negative padding counts as 0.
CropBox paddedBox(const CropBox &bounds, int padding, int imageWidth, int imageHeight);

// The longest side Crop to Mask makes, whatever the megapixels and the box's shape.
constexpr int kMaxCropSide = 4096;

// The size a box is redrawn at: Resize to Megapixels' size for the box's shape (each side a
// multiple of `multiple`, at least one step, none above kMaxCropSide) when `megapixels` is above 0, else the box's own size.
CropSize cropSizeFor(const CropBox &box, double megapixels, int multiple);

// `source` cut to `box` (which must lie inside it).
Pixels cutBox(const Pixels &source, const CropBox &box);

struct Cropped {
    CropBox box;     // where the crop came from, in the picture
    CropSize size;   // the size of `image` and `mask`
    Pixels image;    // the box, resized to `size` (lanczos)
    Pixels mask;     // the mask's box, resized to `size` (bilinear); one channel
};

// The part of `image` and `mask` (the picture's size) inside `box`, redrawn at `size`: what Crop to
// Mask and Canvas Region both cut. The mask is clamped to 0..1 after the bilinear resize.
Cropped cropAt(const Pixels &image, const Pixels &mask, const CropBox &box, const CropSize &size);

// Crop to Mask: the box of `mask` (which has the picture's size) grown by `padding`, cut out and
// resized as cropSizeFor says. None for a mask with nothing in it.
std::optional<Cropped> cropToMask(const Pixels &image, const Pixels &mask, int padding, double megapixels, int multiple);

// Whether a picture of width x height has the shape of `size`, within 2% of its aspect ratio
// (Stitch's `stitch_mismatch`).
bool shapeFits(const CropSize &size, int width, int height);

// Stitch: `redrawn` (any size of the crop's shape) resized to the box (lanczos; as it is when it is
// that size already) and laid over `original` through `mask` (the original's size) feathered
// outward by `blend` pixels and cut at the box. Outside the box, and wherever the mask stays 0,
// `original` is returned as it was.
Pixels stitch(const Pixels &original, const Pixels &mask, const CropBox &box, const Pixels &redrawn, int blend);

} // namespace fugue::nodes
