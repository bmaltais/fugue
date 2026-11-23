#include "canvas/region_pick.h"

#include <algorithm>
#include <cmath>

#include "nodes/canvas_region_ops.h"

namespace fugue::region {

namespace {

nodes::CropBox boxOf(const crop::Rect &r) { return {r.x, r.y, r.width, r.height}; }
crop::Rect rectOf(const nodes::CropBox &b) { return {b.x, b.y, b.width, b.height}; }

// One axis of snapped(): the span [start, start + length) in 0..limit.
void snapAxis(int start, int length, int limit, int step, bool keepSize, int *outStart, int *outLength) {
    const auto nearest = [step](int v) { return int(std::lround(double(v) / step)) * step; };
    if (keepSize) {
        *outStart = std::clamp(nearest(start), 0, std::max(0, limit - length));
        *outLength = length;
        return;
    }
    int a = std::clamp(nearest(start), 0, limit);
    int end = start + length;
    int b = nearest(end);
    // The canvas's own far edge is a place to stop, when it is nearer than a multiple.
    if (std::abs(limit - end) < std::abs(b - end) || b > limit)
        b = limit;
    if (b <= a) {
        b = std::min(limit, a + step);
        if (b <= a) { // at the far edge: take the last step before it
            a = std::max(0, (limit - 1) / step * step);
            b = limit;
        }
    }
    *outStart = a;
    *outLength = b - a;
}

} // namespace

crop::Rect initial(int x, int y, int width, int height, const crop::Size &canvas) {
    if (const auto box = nodes::rectBox(x, y, width, height, canvas.width, canvas.height))
        return rectOf(*box);
    return {0, 0, canvas.width, canvas.height};
}

crop::Rect snapped(const crop::Rect &rect, int multiple, const crop::Size &canvas, bool keepSize) {
    if (multiple <= 1 || canvas.width <= 0 || canvas.height <= 0)
        return rect;
    crop::Rect out;
    snapAxis(rect.x, rect.width, canvas.width, multiple, keepSize, &out.x, &out.width);
    snapAxis(rect.y, rect.height, canvas.height, multiple, keepSize, &out.y, &out.height);
    return out;
}

crop::Rect context(const crop::Rect &rect, int padding, const crop::Size &canvas, int feather) {
    return rectOf(nodes::paddedBox(boxOf(rect), nodes::paddingFor(padding, feather), canvas.width, canvas.height));
}

crop::Size modelSize(const crop::Rect &rect, const Settings &settings, const crop::Size &canvas) {
    const nodes::CropSize size = nodes::regionSize(boxOf(context(rect, settings.padding, canvas, settings.feather)), settings.scale,
                                                   settings.capMegapixels, settings.multiple, settings.targetMegapixels);
    return {size.width, size.height};
}

bool capped(const crop::Rect &rect, const Settings &settings, const crop::Size &canvas) {
    Settings uncapped = settings;
    uncapped.capMegapixels = 0;
    const crop::Size with = modelSize(rect, settings, canvas), without = modelSize(rect, uncapped, canvas);
    return with.width * with.height < without.width * without.height;
}

} // namespace fugue::region
