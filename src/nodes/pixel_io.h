#pragma once

// Reading and writing the float pixel buffers of the pixel and mask nodes (spec 14.1, 14.4): a
// node input as Pixels, and Pixels as a node output. Shared by pixel.cpp, mask.cpp and the nodes
// that come after them (Stitch, Generate Image's composite).

#include <QString>

#include "engine/node_kernel.h"
#include "nodes/resample.h"

namespace fugue::nodes {

// The image an input holds as float RGB(A) in 0..1: gray becomes RGB (with alpha), more than four
// bands keep the first three. `missing_input` when the input holds no image.
Result<Pixels> pixelsOf(const Value &value, const QString &input);

// `pixels` clamped to 0..1 as the output "image".
Result<Values> imageOutput(Pixels pixels);

// A number input as a whole number, held within plus or minus a billion so a huge value cannot
// wrap around; not a number is 0.
int toInt(const Value &value);

// A mask input as one channel: an image's first channel (a gray mask is its own).
Result<Pixels> maskOf(const Value &value, const QString &input);

// A result image under the output name `name` ("image", or "mask" for a one-channel mask).
Result<Values> namedOutput(Pixels pixels, const char *name);

} // namespace fugue::nodes
