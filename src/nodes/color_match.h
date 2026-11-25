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

// Match Color (#555), after Photoshop's Image > Adjustments > Match Color as cubiq's ImageColorMatchAdobe+ (ComfyUI_essentials,
// MIT) reads it; Photoshop's own formula is unpublished. Both pictures go to CIELAB (D65); with the statistics of the
// reference (weighted as above) and of the image: L moves to the reference's mean and is scaled by the ratio of the standard
// deviations times `luminance`; `neutralize` first takes that fraction of the image's own mean a and b off (its color cast);
// a and b are then scaled together by the ratio of the chroma spreads sqrt(var a + var b), times `colorIntensity`. The
// result is mixed with the image by `strength` (the fade) and clamped. A ratio whose image spread is 0 is 1. Alpha is kept.
struct MatchColorOptions {
    float luminance = 1.0f;      // 0 to 2
    float colorIntensity = 1.0f; // 0 to 2
    float neutralize = 0.0f;     // 0 to 1
};
Pixels matchColor(const Pixels &image, const Pixels &reference, const Pixels *weights, float strength,
                  const MatchColorOptions &options, bool *allMasked = nullptr);

} // namespace fugue::nodes
