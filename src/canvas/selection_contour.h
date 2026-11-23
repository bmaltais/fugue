#pragma once

// The outline of a mask at the view's resolution (#313, the marching ants): marching squares on a grid of nodes
// `cell` picture pixels apart, aligned to multiples of `cell` (so the outline does not shift as the view pans), limited
// to a window of the picture (what the view shows). The 0.5 level (127.5 of 255) is where a segment crosses a grid
// edge, interpolated, so a hard mask gives the pixel boundary and a feathered one its middle. The result is a list of
// polylines in picture pixels, a closed one ending on its first point. The work is the window's node count, never the
// picture's: a 16384 px canvas costs what a 1000 px one does at the same zoom.

#include <QPointF>
#include <QRect>

#include <cstdint>
#include <functional>
#include <vector>

namespace fugue {

using ContourPath = std::vector<QPointF>;

// The picture-pixel step that keeps a node about `minScreen` screen pixels from the next at `zoom` (a power of two, at least 1).
int contourCell(double zoom, double minScreen = 2);

// `value(x, y)` is the mask at a picture pixel (0 to 255) inside width x height; outside it is 0. `window` is the part of
// the picture to outline (picture pixels; clipped to it).
std::vector<ContourPath> selectionContour(int width, int height, const std::function<uint8_t(int, int)> &value,
                                          const QRect &window, int cell);

} // namespace fugue
