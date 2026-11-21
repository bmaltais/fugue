#include "engine/crop_geometry.h"

#include <cstdlib>

namespace fugue::crop {

namespace {

int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(v, std::max(lo, hi))); }

int roundInt(double v) { return int(std::lround(v)); }

} // namespace

Rect applied(const Rect &rect, const Size &image) {
    const int x = clampInt(rect.x, 0, image.width - 1);
    const int y = clampInt(rect.y, 0, image.height - 1);
    return {x, y, std::min(std::max(rect.width, 1), std::max(1, image.width - x)),
            std::min(std::max(rect.height, 1), std::max(1, image.height - y))};
}

Rect clamped(const Rect &rect, const Size &image) {
    const int width = clampInt(rect.width, 1, image.width);
    const int height = clampInt(rect.height, 1, image.height);
    return {clampInt(rect.x, 0, image.width - width), clampInt(rect.y, 0, image.height - height), width, height};
}

Rect moved(const Rect &start, int dx, int dy, const Size &image) {
    return clamped({start.x + dx, start.y + dy, start.width, start.height}, image);
}

namespace {

struct Sides {
    bool left = false, right = false, top = false, bottom = false;
};

Sides sidesOf(Handle handle) {
    switch (handle) {
    case Handle::TopLeft: return {true, false, true, false};
    case Handle::Top: return {false, false, true, false};
    case Handle::TopRight: return {false, true, true, false};
    case Handle::Right: return {false, true, false, false};
    case Handle::BottomRight: return {false, true, false, true};
    case Handle::Bottom: return {false, false, false, true};
    case Handle::BottomLeft: return {true, false, false, true};
    case Handle::Left: return {true, false, false, false};
    default: return {};
    }
}

// One axis, free: the edges [lo, hi] of a span of `length` in an image `limit` long, one edge (or both,
// from the center) moved by `delta`. Returns the whole-pixel span start and size.
void freeAxis(int start, int size, int limit, bool low, bool high, int delta, bool fromCenter, int &outStart, int &outSize) {
    if (!low && !high) {
        outStart = start;
        outSize = size;
        return;
    }
    const double center = start + size / 2.0;
    double a = start, b = start + size; // the edges
    if (low)
        a += delta;
    if (high)
        b += delta;
    if (fromCenter) {
        if (low)
            b = 2 * center - a;
        else
            a = 2 * center - b;
        // Within the image on both sides, and one pixel wide at least.
        const double half = std::min(center, limit - center);
        a = std::max(a, center - half);
        b = std::min(b, center + half);
        if (low) {
            a = std::min(a, center - 0.5);
            b = 2 * center - a;
        } else {
            b = std::max(b, center + 0.5);
            a = 2 * center - b;
        }
    } else if (low) {
        a = std::max(0.0, std::min(a, b - 1));
    } else {
        b = std::min(double(limit), std::max(b, a + 1));
    }
    int ia = roundInt(a), ib = roundInt(b);
    if (ib <= ia) {
        if (low)
            ia = ib - 1;
        else
            ib = ia + 1;
    }
    ia = std::max(ia, 0);
    ib = std::min(ib, limit);
    if (ib <= ia)
        ib = ia + 1;
    outStart = ia;
    outSize = ib - ia;
}

} // namespace

Rect resized(const Rect &start, Handle handle, int dx, int dy, const Size &image, double ratio, bool fromCenter) {
    const Sides s = sidesOf(handle);
    if (!(s.left || s.right || s.top || s.bottom))
        return start;
    const Rect from = clamped(start, image);
    if (ratio <= 0) {
        Rect out = from;
        freeAxis(from.x, from.width, image.width, s.left, s.right, dx, fromCenter, out.x, out.width);
        freeAxis(from.y, from.height, image.height, s.top, s.bottom, dy, fromCenter, out.y, out.height);
        return out;
    }

    // Ratio lock. The wanted size from the raw drag, then limited by the room the anchors leave.
    const double cx = from.x + from.width / 2.0, cy = from.y + from.height / 2.0;
    const bool centerX = fromCenter || !(s.left || s.right);
    const bool centerY = fromCenter || !(s.top || s.bottom);
    double rawW = from.width, rawH = from.height;
    if (s.left)
        rawW -= dx;
    if (s.right)
        rawW += dx;
    if (s.top)
        rawH -= dy;
    if (s.bottom)
        rawH += dy;
    if (fromCenter) {
        // The opposite side mirrors: the drag counts twice.
        if (s.left)
            rawW -= dx;
        if (s.right)
            rawW += dx;
        if (s.top)
            rawH -= dy;
        if (s.bottom)
            rawH += dy;
    }
    double w;
    if ((s.left || s.right) && (s.top || s.bottom))
        w = rawW / ratio >= rawH ? rawW : rawH * ratio; // a corner: the larger drag
    else if (s.left || s.right)
        w = rawW;
    else
        w = rawH * ratio;

    const double maxW = centerX ? 2 * std::min(cx, image.width - cx)
                                : (s.left ? from.x + from.width : image.width - from.x);
    const double maxH = centerY ? 2 * std::min(cy, image.height - cy)
                                : (s.top ? from.y + from.height : image.height - from.y);
    const double most = std::min(maxW, maxH * ratio);
    w = std::min(std::max(w, std::max(1.0, ratio)), most);
    const int wi = std::max(1, std::min(roundInt(w), image.width));
    const int hi = std::max(1, std::min(roundInt(w / ratio), image.height));

    Rect out{0, 0, wi, hi};
    out.x = centerX ? roundInt(cx - wi / 2.0) : (s.left ? from.x + from.width - wi : from.x);
    out.y = centerY ? roundInt(cy - hi / 2.0) : (s.top ? from.y + from.height - hi : from.y);
    out.x = clampInt(out.x, 0, image.width - wi);
    out.y = clampInt(out.y, 0, image.height - hi);
    return out;
}

Rect fitRatio(const Rect &rect, double ratio, const Size &image) {
    const Rect r = clamped(rect, image);
    if (ratio <= 0)
        return r;
    int w = r.width, h = r.height;
    if (double(w) / h > ratio)
        w = std::max(1, roundInt(h * ratio));
    else
        h = std::max(1, roundInt(w / ratio));
    return clamped({r.x + roundInt((r.width - w) / 2.0), r.y + roundInt((r.height - h) / 2.0), w, h}, image);
}

double ratioOf(const std::string &name, const Size &image) {
    if (name.empty() || name == "free")
        return 0;
    if (name == "source")
        return image.width > 0 && image.height > 0 ? double(image.width) / image.height : 0;
    const size_t colon = name.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= name.size())
        return 0;
    char *end = nullptr;
    const std::string left = name.substr(0, colon), right = name.substr(colon + 1);
    const double a = std::strtod(left.c_str(), &end);
    if (*end != '\0')
        return 0;
    const double b = std::strtod(right.c_str(), &end);
    if (*end != '\0')
        return 0;
    return a > 0 && b > 0 ? a / b : 0;
}

Handle handleAt(const Rect &frame, double px, double py, double grab) {
    const double gx = std::min(grab, frame.width / 3.0), gy = std::min(grab, frame.height / 3.0);
    const double x0 = frame.x, x1 = frame.x + frame.width, y0 = frame.y, y1 = frame.y + frame.height;
    if (px < x0 - gx || px > x1 + gx || py < y0 - gy || py > y1 + gy)
        return Handle::None;
    const bool l = std::abs(px - x0) <= gx, r = std::abs(px - x1) <= gx;
    const bool t = std::abs(py - y0) <= gy, b = std::abs(py - y1) <= gy;
    if (l && t) return Handle::TopLeft;
    if (r && t) return Handle::TopRight;
    if (l && b) return Handle::BottomLeft;
    if (r && b) return Handle::BottomRight;
    if (t) return Handle::Top;
    if (b) return Handle::Bottom;
    if (l) return Handle::Left;
    if (r) return Handle::Right;
    return Handle::Inside;
}

Rect Mapping::imageRect(double vx0, double vy0, double vx1, double vy1, const Size &image) const {
    const Point a = toImage(vx0, vy0), b = toImage(vx1, vy1);
    const int x0 = clampInt(roundInt(std::min(a.x, b.x)), 0, image.width - 1);
    const int y0 = clampInt(roundInt(std::min(a.y, b.y)), 0, image.height - 1);
    const int x1 = clampInt(roundInt(std::max(a.x, b.x)), x0 + 1, image.width);
    const int y1 = clampInt(roundInt(std::max(a.y, b.y)), y0 + 1, image.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

Mapping fitted(const Size &image, double boxWidth, double boxHeight) {
    if (image.width <= 0 || image.height <= 0 || boxWidth <= 0 || boxHeight <= 0)
        return {};
    const double scale = std::min(boxWidth / image.width, boxHeight / image.height);
    return {(boxWidth - image.width * scale) / 2, (boxHeight - image.height * scale) / 2, scale};
}

} // namespace fugue::crop
