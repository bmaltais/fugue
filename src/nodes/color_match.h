#pragma once

// Color Match (#180, spec 14.4): move a picture's colors to a reference's, to hide the color shift
// at the seam of an inpaint or an outpaint. Plain functions on float pixel buffers, no libvips and
// no node types, as mask_ops.h.

#include "nodes/resample.h"

namespace fugue::nodes {

// `image` with its colors matched to `reference`: Reinhard, Ashikhmin, Gooch and Shirley, "Color
// Transfer between Images" (2001). Both pictures go to the l-alpha-beta space (linear light, then the
// paper's LMS cone space in logs, then its decorrelated axes); each axis of `image` is moved to the
// reference's mean and scaled to its standard deviation; the result goes back. `strength` 0 is the
// image, 1 the full transfer, between them a mix in that space.
//
// `weights` (one channel, the image's size, or null for 1 everywhere) says how much each pixel of
// `image` counts in its statistics, and in the reference's when that has the image's size too (the
// two then describe the same place); a reference of another size is measured over all its pixels.
// An axis with no spread in `image` is only moved (scale 1). When every weight is 0 the statistics
// fall back to all pixels and `*allMasked` is set. The alpha channel is kept as it is.
Pixels colorMatch(const Pixels &image, const Pixels &reference, const Pixels *weights, float strength,
                  bool *allMasked = nullptr);

} // namespace fugue::nodes
