#include "canvas/transform_math.h"

#include <algorithm>
#include <cmath>

namespace fugue::xform {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinSide = 1;

struct Sign {
    int x, y; // which side a handle is on: -1 left or top, 1 right or bottom, 0 the middle
};

Sign signOf(Handle h) {
    switch (h) {
    case Handle::TopLeft: return {-1, -1};
    case Handle::Top: return {0, -1};
    case Handle::TopRight: return {1, -1};
    case Handle::Right: return {1, 0};
    case Handle::BottomRight: return {1, 1};
    case Handle::Bottom: return {0, 1};
    case Handle::BottomLeft: return {-1, 1};
    case Handle::Left: return {-1, 0};
    default: return {0, 0};
    }
}

crop::Point toLocal(const canvas::LayerTransform &t, const crop::Point &p) { // canvas point -> the box's own frame, origin at its center
    const double a = t.angle * kPi / 180.0, c = std::cos(a), s = std::sin(a);
    const double dx = p.x - t.cx, dy = p.y - t.cy;
    return {dx * c + dy * s, -dx * s + dy * c};
}

crop::Point toCanvas(const canvas::LayerTransform &t, const crop::Point &local) {
    const double a = t.angle * kPi / 180.0, c = std::cos(a), s = std::sin(a);
    return {t.cx + local.x * c - local.y * s, t.cy + local.x * s + local.y * c};
}

double limited(double side) { return std::clamp(side, kMinSide, double(canvas::maxTransformSide)); }

} // namespace

bool handleFromName(const QString &name, Handle *handle) {
    static const std::pair<const char *, Handle> names[] = {
        {"top-left", Handle::TopLeft}, {"top", Handle::Top}, {"top-right", Handle::TopRight}, {"right", Handle::Right}, {"bottom-right", Handle::BottomRight},
        {"bottom", Handle::Bottom}, {"bottom-left", Handle::BottomLeft}, {"left", Handle::Left}, {"move", Handle::Move}, {"rotate", Handle::Rotate}};
    for (const auto &[text, value] : names)
        if (name == QLatin1String(text)) {
            *handle = value;
            return true;
        }
    return false;
}

canvas::LayerTransform dragged(const canvas::LayerTransform &start, Handle handle, const crop::Point &from, const crop::Point &to, Keys keys) {
    canvas::LayerTransform t = start;
    if (handle == Handle::Move) {
        double dx = to.x - from.x, dy = to.y - from.y;
        if (keys.shift) {
            if (std::abs(dx) >= std::abs(dy))
                dy = 0;
            else
                dx = 0;
        }
        t.cx += dx;
        t.cy += dy;
        return t.normalized();
    }
    if (handle == Handle::Rotate) {
        const double before = std::atan2(from.y - start.cy, from.x - start.cx), after = std::atan2(to.y - start.cy, to.x - start.cx);
        double angle = start.angle + (after - before) * 180.0 / kPi;
        if (keys.shift)
            angle = std::round(angle / 15.0) * 15.0;
        t.angle = angle;
        return t.normalized();
    }
    const Sign sign = signOf(handle);
    if (sign.x == 0 && sign.y == 0)
        return t;
    const double W = start.width, H = start.height;
    // Where the grabbed handle is now, in the box's own frame: where it was plus how far the pointer has gone along the box's axes.
    const crop::Point a = toLocal(start, from), b = toLocal(start, to);
    const double hx = sign.x * W / 2 + (b.x - a.x), hy = sign.y * H / 2 + (b.y - a.y);
    // Each axis: the side opposite the handle holds (the center, with Alt); the new side follows the handle.
    double newW = W, newH = H;
    if (sign.x != 0)
        newW = limited(keys.alt ? 2 * sign.x * hx : sign.x * hx + W / 2);
    if (sign.y != 0)
        newH = limited(keys.alt ? 2 * sign.y * hy : sign.y * hy + H / 2);
    if (keys.shift) { // the proportions of the start hold: a corner follows the larger drag, an edge sets the other side to match
        double scale = sign.x != 0 && sign.y != 0 ? std::max(newW / W, newH / H) : sign.x != 0 ? newW / W : newH / H;
        scale = std::max(scale, std::max(kMinSide / W, kMinSide / H));
        newW = limited(W * scale);
        newH = limited(H * scale);
    }
    // The held side (or corner) stays put, so the center moves half the change toward the handle; with Alt the center holds. An edge
    // handle keeps the other axis centered.
    const double centreX = sign.x != 0 && !keys.alt ? sign.x * (newW - W) / 2 : 0;
    const double centreY = sign.y != 0 && !keys.alt ? sign.y * (newH - H) / 2 : 0;
    t.width = newW;
    t.height = newH;
    const crop::Point centre = toCanvas(start, {centreX, centreY});
    t.cx = centre.x;
    t.cy = centre.y;
    return t.normalized();
}

std::vector<crop::Point> corners(const canvas::LayerTransform &t) {
    const double w = std::round(t.width) / 2, h = std::round(t.height) / 2;
    return {toCanvas(t, {-w, -h}), toCanvas(t, {w, -h}), toCanvas(t, {w, h}), toCanvas(t, {-w, h})};
}

crop::RectF bounds(const canvas::LayerTransform &t) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const crop::Point &p : corners(t)) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

canvas::LayerTransform snapped(const canvas::LayerTransform &start, const canvas::LayerTransform &placement, Handle handle, Keys keys, const snap::Targets &targets) {
    canvas::LayerTransform t = placement;
    if (targets.threshold <= 0)
        return t;
    if (handle == Handle::Move) {
        const crop::RectF box = bounds(placement), pulled = snap::snapFloating(box, targets, true);
        t.cx += pulled.x - box.x;
        t.cy += pulled.y - box.y;
        return t.normalized();
    }
    const Sign sign = signOf(handle);
    if ((sign.x == 0 && sign.y == 0) || keys.shift || keys.alt || start.angle != 0 || placement.angle != 0)
        return t;
    // An unturned box: only the edges the handle moves are pulled; the held ones stay where they were.
    crop::RectF box{placement.cx - placement.width / 2, placement.cy - placement.height / 2, placement.width, placement.height};
    const crop::RectF pulled = snap::snapFloating(box, targets, false);
    double left = box.x, right = box.x + box.width, top = box.y, bottom = box.y + box.height;
    if (sign.x < 0)
        left = pulled.x;
    if (sign.x > 0)
        right = pulled.x + pulled.width;
    if (sign.y < 0)
        top = pulled.y;
    if (sign.y > 0)
        bottom = pulled.y + pulled.height;
    if (right - left >= kMinSide && bottom - top >= kMinSide) {
        t.width = right - left;
        t.height = bottom - top;
        t.cx = (left + right) / 2;
        t.cy = (top + bottom) / 2;
    }
    return t.normalized();
}

} // namespace fugue::xform
