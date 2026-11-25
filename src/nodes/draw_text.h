#pragma once

// Draw Text (#549, spec 14.2): write text on a picture. Plain functions on float pixel buffers; the
// glyphs come from nodes/text_raster.h.

#include <array>

#include "nodes/resample.h"

namespace fugue::nodes {

// Where the text block sits at (x, y): which point of the block is put there.
enum class TextAnchor { TopLeft, TopCenter, TopRight, CenterLeft, Center, CenterRight, BottomLeft, BottomCenter, BottomRight };

struct DrawTextStyle {
    int x = 0;
    int y = 0;
    TextAnchor anchor = TextAnchor::TopLeft;
    std::array<float, 4> color = {1, 1, 1, 1};        // red, green, blue, alpha
    int outline = 0;                                    // px, around every glyph
    std::array<float, 4> outlineColor = {0, 0, 0, 1};
    int boxPadding = 0;                                 // px, around the text and its outline
    std::array<float, 4> boxColor = {0, 0, 0, 0};       // alpha 0: no box
};

// `image` with the text whose coverage is `coverage` (one channel, from rasterizeText) drawn over it:
// the box first, then the outline, then the text, each blended over what is there with its color's
// alpha times its coverage. An RGBA picture gets the usual "over" alpha; an RGB one stays RGB. Text
// outside the picture is clipped. An empty coverage gives the picture.
Pixels drawText(const Pixels &image, const Pixels &coverage, const DrawTextStyle &style);

} // namespace fugue::nodes
