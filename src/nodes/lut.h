#pragma once

// Apply LUT (#548, spec 14.2): colour grading from an Adobe/Resolve `.cube` file. Plain functions
// on float pixel buffers, as color_match.h: the parser, and the lookup.

#include <QString>

#include <vector>

#include "engine/result.h"
#include "nodes/resample.h"

namespace fugue::nodes {

// A parsed `.cube` look-up table. `size` entries per axis (1D: size entries in all; 3D: size^3, the
// red index running fastest, as the format says); each entry is an RGB triple. The domain maps an
// input value to the table's 0..1 index range, per channel.
struct Lut {
    bool is3d = false;
    int size = 0;
    float domainMin[3] = {0, 0, 0};
    float domainMax[3] = {1, 1, 1};
    std::vector<float> table;
};

// Parses the text of a `.cube` file: TITLE, DOMAIN_MIN, DOMAIN_MAX, LUT_1D_SIZE or LUT_3D_SIZE (one
// of them), comments (#) and the data rows. A malformed file is an error whose message names the line
// ("line 7: ..."), or the file as a whole when it is short of rows.
Result<Lut> parseCube(const QString &text);

// `image` through `lut` (linear interpolation for 1D, trilinear for 3D, an input outside the domain
// clamped to it), mixed with the image by `strength` (0 is the image). The alpha channel is kept.
Pixels applyLut(const Pixels &image, const Lut &lut, float strength);

} // namespace fugue::nodes
