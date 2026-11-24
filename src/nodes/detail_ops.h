#pragma once

// The arithmetic of Detail Regions (spec 14.3, #345): which boxes become crops, the size each is
// redrawn at, the seed and denoise of each, and the feathered paste back. Plain functions on
// numbers and float buffers: no libvips, no node types, no model. The node in components.cpp
// calls these between its jobs.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nodes/crop_stitch_ops.h"

namespace fugue::nodes {

// A detected box as Find by Text writes it: the corners x1 y1 x2 y2 in the picture's pixels.
struct Corners {
    double x1 = 0;
    double y1 = 0;
    double x2 = 0;
    double y2 = 0;
};

// The boxes of `text`, one per line as `x1 y1 x2 y2` (blank lines are skipped, numbers may be
// fractions). Nothing and `problem` set (the line's number and what is wrong) for a line that is
// not four numbers.
std::optional<std::vector<Corners>> parseBoxes(const std::string &text, std::string *problem);

// A crop is never smaller than this on a side, as the reference's skip.
constexpr int kMinCropSide = 16;

// The box's corners rounded to whole pixels, grown by `padding` on each side and cut at the
// picture's edge (not moved: a box at the edge stays short on that side). Nothing when the result
// is smaller than kMinCropSide on a side.
std::optional<CropBox> detailCrop(const Corners &box, int padding, int imageWidth, int imageHeight);

// The size a crop is redrawn at: the long side `targetSize` and the short side in the crop's
// shape, each a multiple of `multiple` (the short side at least 64, never below one step).
CropSize detailSize(const CropBox &crop, int targetSize, int multiple);

enum class DenoiseScale { Fixed, SmallerLess, SmallerMore };

// The denoise for a box of `boxArea` pixels in a picture of `imageArea`: ratio =
// clamp(boxArea / imageArea, 0.01, 0.5) / 0.5; smaller_less multiplies by 0.4 + 0.6 ratio (a small
// box gets less), smaller_more by 1 - 0.6 ratio (a small box gets more); clamped to 0.05..1 and
// rounded to three places. Fixed returns `base` as it is.
double scaledDenoise(double base, DenoiseScale scale, double boxArea, double imageArea);

// The seed of the box at `index` in the list given (skipped boxes count): seed + shift * 100000 +
// index, wrapped at 64 bits.
std::int64_t detailSeed(std::int64_t seed, std::int64_t shift, int index);

// The weight of each pixel of a crop of width x height when it is pasted: 1 inside, and on each of
// the four edges a linear ramp from 0 at the edge to 1, `f` pixels long, f = min(feather, height /
// 2, width / 2). The ramps of two edges multiply in a corner. One channel.
Pixels featherWeights(int width, int height, int feather);

// `redrawn` (the crop's size) laid over `canvas` at `box` through featherWeights. Only the first
// `redrawn.channels` channels of the canvas change: an alpha channel of the picture is kept.
void pasteFeathered(Pixels &canvas, const CropBox &box, const Pixels &redrawn, int feather);

} // namespace fugue::nodes
