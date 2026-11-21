#pragma once

// The mask a node paints (spec 14.4, #174): an input of widget `mask` holds the path of an 8-bit gray
// PNG in the inputs folder, and Load Image and Paint Mask turn it into their `mask` output. Plain
// functions the two nodes share, and the Paint Mask kernel.

#include "engine/media.h"
#include "engine/node_kernel.h"
#include "nodes/resample.h"

namespace fugue::nodes {

// Load Image's `mask` output for `picture` (spec 14.4): the painted mask when the `mask` input names
// one; else 1 - alpha when the picture has an alpha channel; else all 0. A painted mask of another
// size than the picture is scaled to it (bilinear) with the warning `mask_resized`, added to
// `warnings`. The mask as one channel of the picture's size.
Result<Pixels> loadImageMask(const NodeContext &context, const MediaHandle &picture, Errors &warnings);

// Paint Mask's output: the painted mask at the size of `picture`, all 0 for none.
Result<Pixels> paintedMask(const NodeContext &context, const MediaHandle &picture, Errors &warnings);

// Registers fugue/paint-mask.
void addMaskInputKernels(Kernels &kernels);

} // namespace fugue::nodes
