#pragma once

// The arithmetic of the mask and composite nodes (spec 14.4), on float pixel buffers and nothing
// else: no libvips, no node types. A mask is a Pixels with one channel, 1 the part to change.
// The nodes in mask.cpp call these; Stitch (#176) and Generate Image (#175) call featherMask and
// compositeMasked directly.

#include <array>

#include "engine/edge_fill.h"
#include "nodes/resample.h"

namespace fugue::nodes {

// `mask` stretched to width x height (torch's bilinear interpolate); the mask itself when it is
// that size already.
Pixels maskResized(const Pixels &mask, int width, int height);

// ComfyUI's ImageCompositeMasked: `source` pasted at x, y on `destination`, out = d * (1 - m) +
// s * m, where it lies inside the destination (x and y may be negative or past the edge: what
// sticks out is cut, and a source wholly outside leaves the destination as it is). `mask` is one
// channel, or null for all 1; of another size than the source it is stretched to it. The result has
// the destination's channels; a source without a channel (an alpha) gives 1 for it.
Pixels compositeMasked(const Pixels &destination, const Pixels &source, const Pixels *mask, int x, int y);

// ComfyUI's ThresholdMask: 1 where the mask is above `value`, else 0.
Pixels thresholdMask(const Pixels &mask, float value);

enum class CombineOperation { Multiply, Add, Subtract, And, Or, Xor };

// ComfyUI's MaskComposite: `source` combined into `destination` at x, y over the part that
// overlaps (the same cut as compositeMasked), then clamped to 0..1. and, or and xor round to 0 or 1
// first (half to even, as torch.round does).
Pixels combineMasks(const Pixels &destination, const Pixels &source, CombineOperation operation, int x, int y);

enum class FeatherDirection { Outward, Inward, Centered };

// A soft edge `radius` pixels wide: the mask blurred by three box passes (each box
// with the variance radius^2 / 12, so the three together have the variance of a Gaussian of sigma
// radius / 2; the ends of a box take a fraction of a pixel, so the width is continuous), the picture's edge
// repeated beyond it. Outward is max(m, blurred), inward min(m, blurred), centered the blurred
// mask. Radius 0 is the mask. Linear in the number of pixels, whatever the radius.
Pixels featherMask(const Pixels &mask, int radius, FeatherDirection direction);

enum class FillMode { Gray, Blur };

// `image` with what lies under `mask` filled: gray is 0.5 (alpha 1); blur spreads the colors of the
// pixels the mask leaves (weight 1 - m) into it, as averages from a pyramid of coarser pictures,
// so a filled pixel stays within the range of the colors around it (all gray when nothing is left).
// The mask is stretched to the image's size. Where m = 0 the pixel is returned as it was; elsewhere
// out = image * (1 - m) + fill * m.
Pixels fillMasked(const Pixels &image, const Pixels &mask, FillMode mode);

// `image` with what lies under `mask` filled from its edge (#243, engine/edge_fill.h, shared with Pad and the canvas
// resize): each masked pixel takes the colour of the nearest pixel the mask leaves (m = 0), stretched outward, and for
// EdgeBlur blurred by `blur` px (the picture itself is not blurred). Gray and Transparent are the fills' other
// modes. Better than fillMasked's blur for a hole open on one side (an outpaint strip), where a blur across it pulls
// the far side in. The mask is stretched to the image's size; where m = 0 the pixel is as it was, elsewhere
// out = image * (1 - m) + fill * m. A picture with nothing left outside the mask is all gray.
Pixels extendEdges(const Pixels &image, const Pixels &mask, edgefill::Mode mode, int blur);

// `image` with `color` (RGB, 0..1) laid over it at mask * opacity. The mask is stretched to the
// image's size; an alpha channel is left alone.
Pixels overlayMask(const Pixels &image, const Pixels &mask, const std::array<float, 3> &color, float opacity);

// `image` as RGBA with its alpha replaced by alpha x mask: the colors are kept as they are (not
// multiplied by the mask), so the cutout keeps its edge pixels' own colors. An image without alpha
// counts as opaque. The mask is stretched to the image's
// size.
Pixels maskToAlpha(const Pixels &image, const Pixels &mask);

} // namespace fugue::nodes
