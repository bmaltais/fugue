#pragma once

// Fills a hole in a picture from the picture around it (#240 canvas resize, #243 Pad and Extend Edges).
// The benchmark of docs/inpaint-outpaint-research.md (4.6) found that the new area of an outpaint wants
// the nearest picture stretched into it and then blurred (24 px), which beat gray and transparent. This
// is that, once, on plain float pixels (0 to 1, interleaved, 1 to 4 channels) so the canvas store, the
// Pad node and the Extend Edges node all use the same code. Nothing here knows a node or a canvas.

#include <QString>

#include <cstdint>
#include <vector>

namespace fugue::edgefill {

// What the hole holds afterwards.
//   Gray        0.5 (alpha 1): ComfyUI's Pad Image for Outpainting.
//   Edge        each hole pixel takes the colour of the nearest picture pixel (the picture's edge stretched
//               outward; a corner takes the corner pixel).
//   EdgeBlur    Edge, then blurred so the stretched stripes turn into a soft haze; the picture itself is
//               not touched (the blur reads it, so the haze meets it without a step).
//   Transparent 0 everywhere (alpha 0): only for a picture with alpha, the others get 0.5 gray.
enum class Mode { Gray, Edge, EdgeBlur, Transparent };

inline constexpr int defaultBlur = 24; // the benchmark's radius, in pixels

const char *modeName(Mode mode); // "gray", "edge", "edge-blur", "transparent"
bool modeFromName(const QString &name, Mode *mode);

// A picture: `width` x `height` pixels of `channels` floats each (3 RGB or 4 straight RGBA).
struct Image {
    int width = 0;
    int height = 0;
    int channels = 4;
    std::vector<float> data;
};

// Fills the pixels where `hole[i] != 0` (one byte per pixel, row after row) of `image`. Every other pixel is
// left exactly as it is. `blur` is EdgeBlur's radius (0 gives Edge). A picture with no pixel outside the
// hole has nothing to extend and gets gray. A hole that is the whole picture, or an empty one, is fine.
void fillHole(Image &image, const std::vector<uint8_t> &hole, Mode mode, int blur = defaultBlur);

// The index (y * width + x) of the picture pixel nearest to each pixel (itself where `hole` is 0; -1 for
// every pixel when nothing is outside the hole). Distances are Euclidean, approximated by two sweeps of
// the 8 neighbours, which is exact for the rectangles an outpaint adds. Exposed for the tests.
std::vector<int> nearestKept(int width, int height, const std::vector<uint8_t> &hole);

// `image` blurred (three box passes, the picture's edge repeated beyond it) by sigma radius / 2. An alpha
// channel is premultiplied around the blur so a transparent pixel's colour does not bleed in.
void blurImage(Image &image, int radius);

} // namespace fugue::edgefill
