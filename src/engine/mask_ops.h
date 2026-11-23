#pragma once

// Whole-mask operations behind Select > Modify and Transform Selection (#251, Photoshop's semantics): grow and shrink,
// feather, border, smooth, and a move, scale and rotation. They work on a flat 8-bit mask (255 selected, 0 not,
// row after row), return a new one, and know nothing of Qt Quick or of undo (MaskPaint::assign makes the result one step).
//
// Grow and shrink are the Grow Mask node's own pass (`morphPass`: a 3x3 grey dilation or erosion), run n times.
// The node runs its passes the same way each time (ComfyUI's tapered corners make a diamond); a selection alternates
// a tapered and a full pass, which makes a round-looking octagon, as Photoshop's Expand and Contract do.
//
// The work stays in the part of the mask that can change (its bounds and the reach of the operation), so a small
// selection on a large canvas is quick.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "engine/mask_paint.h"

namespace fugue::mask {

// One pass of the 3x3 grey dilation (`grow`) or erosion over a width x height plane. Neighbors outside the plane are
// ignored; `tapered` leaves the footprint's corners out (a diamond, ComfyUI's GrowMask default).
template <typename T>
void morphPass(const T *source, T *target, int width, int height, bool grow, bool tapered) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            T best = source[size_t(y) * size_t(width) + size_t(x)];
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (tapered && dx != 0 && dy != 0)
                        continue;
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                        continue;
                    const T v = source[size_t(ny) * size_t(width) + size_t(nx)];
                    best = grow ? std::max(best, v) : std::min(best, v);
                }
            target[size_t(y) * size_t(width) + size_t(x)] = best;
        }
}

namespace ops {

// The largest amount (pixels) of grow, shrink, border, feather and smooth: the work is by pixel passes.
constexpr int kMaxAmount = 250;

// The box holding every pixel above 0; empty when there is none.
IRect bounds(const std::vector<uint8_t> &mask, int width, int height);

// Grows the selection by `amount` pixels (negative: shrinks it), round. Soft edges stay soft.
std::vector<uint8_t> expanded(const std::vector<uint8_t> &mask, int width, int height, int amount);

// A Gaussian blur of the edge with the given radius: the selection fades from solid to nothing over about `radius`
// pixels each side of the old edge. The picture's edge does not fade (a selection reaching it stays solid there).
std::vector<uint8_t> feathered(const std::vector<uint8_t> &mask, int width, int height, int radius);

// A band `width` pixels wide centered on the selection's edge (the outer half outside, the inner half inside), soft
// at its edges as the edge was; the inside of the selection is let go.
std::vector<uint8_t> border(const std::vector<uint8_t> &mask, int width, int height, int bandWidth);

// Rounds corners and drops specks: blurs by `radius`, then selects what is more than half selected.
std::vector<uint8_t> smoothed(const std::vector<uint8_t> &mask, int width, int height, int radius);

// A move of (dx, dy) pixels, a scale (1 = as is, per axis) and a turn in degrees (clockwise on screen), the scale
// and the turn about the center of the selection's bounds. Bilinear; what leaves the picture is lost.
struct Placement {
    double dx = 0;
    double dy = 0;
    double scaleX = 1;
    double scaleY = 1;
    double degrees = 0;
    bool identity() const { return dx == 0 && dy == 0 && scaleX == 1 && scaleY == 1 && degrees == 0; }
};
std::vector<uint8_t> transformed(const std::vector<uint8_t> &mask, int width, int height, const Placement &placement);

} // namespace ops
} // namespace fugue::mask
