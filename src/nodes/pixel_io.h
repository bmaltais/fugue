#pragma once

// Reading and writing the float pixel buffers of the pixel and mask nodes (spec 14.1, 14.4): a
// node input as Pixels, and Pixels as a node output. Shared by pixel.cpp, mask.cpp and the nodes
// that come after them (Stitch, Generate Image's composite).

#include <QString>

#include <vips/vips8>

#include "engine/mask_paint.h"
#include "engine/node_kernel.h"
#include "nodes/resample.h"

namespace fugue::nodes {

// The image an input holds as float RGB(A) in 0..1: gray becomes RGB (with alpha), more than four
// bands keep the first three. `missing_input` when the input holds no image.
Result<Pixels> pixelsOf(const Value &value, const QString &input);

// The same for a picture already read (no media handle, so no hash of its file).
Result<Pixels> pixelsFromImage(const vips::VImage &image, const QString &input);

// `pixels` clamped to 0..1 as the output "image".
Result<Values> imageOutput(Pixels pixels);

// A number input as a whole number, held within plus or minus a billion so a huge value cannot
// wrap around; not a number is 0.
int toInt(const Value &value);

// A mask input as one channel: an image's first channel (a gray mask is its own).
Result<Pixels> maskOf(const Value &value, const QString &input);

// Straight 8-bit RGBA from float pixels (gray and RGB get an opaque alpha), the paint canvas's layers
// (engine/mask_paint.h); and back, 4 channels or 1 as the buffer has.
mask::MaskPaint pixelsToLayer(const Pixels &in);
Pixels layerToPixels(const mask::MaskPaint &in);

// A result image under the output name `name` ("image", or "mask" for a one-channel mask).
Result<Values> namedOutput(Pixels pixels, const char *name);

} // namespace fugue::nodes
