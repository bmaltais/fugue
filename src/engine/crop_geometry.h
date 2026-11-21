#pragma once

// The geometry of a crop frame (#73): a rectangle of whole pixels inside an image, moved and resized
// by its eight handles with an optional ratio lock, and the mapping between image pixels and a
// view's coordinates (a node's face, the large view). Plain numbers: it knows nothing of Qt Quick,
// nodes or the canvas, so the frame on a node, the frame in the large view and any other caller
// (a mask editor, a region picker) draw and edit the same rectangle the same way. The Crop node's
// kernel reads the rectangle by the same rule as `applied`.
//
// A drag is computed from the rectangle it started with and the total offset of the pointer, never
// step by step, so going out past an edge and back returns to where the pointer is.

#include <algorithm>
#include <cmath>
#include <string>

namespace fugue::crop {

struct Size {
    int width = 0;
    int height = 0;
    bool operator==(const Size &o) const { return width == o.width && height == o.height; }
};

// A rectangle of whole pixels: x, y of its top left, then its size.
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const Rect &o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
    bool operator!=(const Rect &o) const { return !(*this == o); }
};

struct Point {
    double x = 0;
    double y = 0;
    bool operator==(const Point &o) const { return x == o.x && y == o.y; }
};

struct RectF {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    bool operator==(const RectF &o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
};

enum class Handle { None, Inside, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };

// What the Crop node cuts for these numbers: the origin held inside the image (ComfyUI's ImageCrop),
// the size at one pixel at least and stopping at the edge. The frame is drawn from this, so it
// shows what the node will cut.
Rect applied(const Rect &rect, const Size &image);

// The rectangle with its size kept (at least 1 pixel, at most the image) slid inside the image.
Rect clamped(const Rect &rect, const Size &image);

// The rectangle moved by (dx, dy) pixels, stopping at the image's edges; its size is kept.
Rect moved(const Rect &start, int dx, int dy, const Size &image);

// The rectangle `start` with `handle` dragged by (dx, dy) pixels in total. The opposite corner or
// edge holds (with `fromCenter`, the center holds and the opposite side mirrors); the frame stops
// at the image's edges and at one pixel, and never flips. `ratio` is width / height, or 0 for free:
// a corner follows the larger of the two drags, an edge sets its own dimension and the other
// follows around the frame's middle. Handle::Inside and None return `start`.
Rect resized(const Rect &start, Handle handle, int dx, int dy, const Size &image, double ratio = 0,
             bool fromCenter = false);

// The largest rectangle of `ratio` that fits in `rect`, around its center; `rect` itself for a free
// ratio. What a ratio picked on an existing frame does, as Photoshop's.
Rect fitRatio(const Rect &rect, double ratio, const Size &image);

// width / height of a ratio named "free" (or empty): 0; "source": the image's; "w:h" ("16:9"): w / h.
// 0 for anything else, a zero or negative side included.
double ratioOf(const std::string &name, const Size &image);

// The handle under (px, py), in image pixels. `grab` is how far a handle reaches, in image pixels
// (the caller divides what it wants on screen by the scale); it reaches a little outside the frame
// and is held to a third of a small frame's side so its inside stays to move it. Corners first,
// then edges, then Inside.
Handle handleAt(const Rect &frame, double px, double py, double grab);

// Image pixels to a view and back: pixel (0, 0)'s corner sits at (originX, originY) and one pixel
// is `scale` view units.
struct Mapping {
    double originX = 0;
    double originY = 0;
    double scale = 1;

    Point toView(double px, double py) const { return {originX + px * scale, originY + py * scale}; }
    Point toImage(double vx, double vy) const { return {(vx - originX) / scale, (vy - originY) / scale}; }
    RectF toViewRect(const Rect &r) const {
        return {originX + r.x * scale, originY + r.y * scale, r.width * scale, r.height * scale};
    }
    // The pixel rectangle between two view points (any order), each rounded to the nearest pixel
    // edge, inside the image and at least one pixel.
    Rect imageRect(double vx0, double vy0, double vx1, double vy1, const Size &image) const;
};

// The mapping that fits the whole image in a box of w x h view units, centered.
Mapping fitted(const Size &image, double boxWidth, double boxHeight);

} // namespace fugue::crop
