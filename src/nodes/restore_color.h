#pragma once

// The colour fix of Restore Image (#346, spec 14.1): what puts a restored picture's colours back to the
// input's after a model that redraws them (SeedVR2). Plain functions on float pixel buffers, no
// libvips and no node types, as color_match.h. Written from the SeedVR2 paper's description of its
// wavelet colour correction and the usual CIELAB and histogram-matching definitions; ComfyUI's
// implementation (GPL) was not read.

#include <vector>

#include "nodes/resample.h"

namespace fugue::nodes {

// Wavelet colour reconstruction: the result's high frequencies and the reference's low ones. Both
// are split into `levels` à-trous bands, each a blur with the binomial kernel [1 2 1] / 4 on each
// axis (so 3x3 with [1 2 1; 2 4 2; 1 2 1] / 16) dilated by 2^level, with the edge pixel repeated
// beyond the border. What is left after the last blur is the low-frequency part; the result is
// `content` minus its own low part plus the reference's, clamped to 0..1. The reference is
// resampled (bicubic) to the content's size when it differs. Alpha is passed through from `content`.
Pixels waveletReconstruction(const Pixels &content, const Pixels &reference, int levels = 5);

// Each value of `values` replaced by the value of `reference` at the same rank share (histogram
// matching, equal values share one rank). `reference` may have a different size. Empty input
// stays empty.
void matchHistogram(std::vector<float> &values, std::vector<float> reference);

// The `lab` colour fix: waveletReconstruction, then in CIELAB (sRGB, D65) a and b histogram-matched
// fully to the reference and L = 0.8 * content + 0.2 * matched. A flat picture stays flat.
Pixels labColorFix(const Pixels &content, const Pixels &reference);

// sRGB (0..1) and CIELAB (D65: L 0..100, a and b about -128..127), for the tests of the fix.
struct Lab {
    double l, a, b;
};
Lab rgbToLab(float r, float g, float b);
void labToRgb(const Lab &lab, float *rgb);

} // namespace fugue::nodes
