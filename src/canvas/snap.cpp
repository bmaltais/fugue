#include "canvas/snap.h"

#include <algorithm>
#include <cmath>

#include "canvas/region_pick.h"

namespace fugue::snap {

namespace {

// The line nearest `value` among `lines` and the grid, and how far it is; false when there is none.
bool nearestLine(double value, const std::vector<int> &lines, int spacing, double *line, double *distance) {
    bool found = false;
    for (const int candidate : lines) {
        const double d = std::abs(candidate - value);
        if (!found || d < *distance) {
            *line = candidate;
            *distance = d;
            found = true;
        }
    }
    if (spacing > 0) {
        const double grid = std::round(value / spacing) * spacing;
        const double d = std::abs(grid - value);
        if (!found || d < *distance) { // a guide wins a tie
            *line = grid;
            *distance = d;
            found = true;
        }
    }
    return found;
}

// One axis of snapRect: the span [start, start + length) in 0..limit pulled to the lines. An edge no line is near stays
// where the multiple put it (`baseStart`, `baseLength`: the span as region::snapped left it, or the span itself).
void pullAxis(int start, int length, int baseStart, int baseLength, int limit, const std::vector<int> &lines, int spacing, double threshold,
              bool keepSize, int *outStart, int *outLength) {
    double a = 0, da = 0, b = 0, db = 0;
    const bool hitA = nearestLine(start, lines, spacing, &a, &da) && da <= threshold;
    const bool hitB = nearestLine(start + length, lines, spacing, &b, &db) && db <= threshold;
    if (keepSize) {
        // The edge that is nearer to a line moves the whole rectangle.
        int at = baseStart;
        if (hitA && (!hitB || da <= db))
            at = int(std::lround(a));
        else if (hitB)
            at = int(std::lround(b)) - length;
        *outStart = std::clamp(at, 0, std::max(0, limit - length));
        *outLength = length;
        return;
    }
    int from = hitA ? int(std::lround(a)) : baseStart;
    int to = hitB ? int(std::lround(b)) : baseStart + baseLength;
    from = std::clamp(from, 0, limit);
    to = std::clamp(to, 0, limit);
    if (to <= from) { // a pull that would close the rectangle is not taken: the span stays as the multiple left it
        from = std::clamp(baseStart, 0, limit);
        to = std::clamp(baseStart + baseLength, 0, limit);
        if (to <= from)
            to = std::min(limit, from + 1);
    }
    *outStart = from;
    *outLength = to - from;
}

} // namespace

double snapCoordinate(double value, const std::vector<int> &lines, int spacing, double threshold, bool *hit) {
    double line = 0, distance = 0;
    const bool found = threshold > 0 && nearestLine(value, lines, spacing, &line, &distance) && distance <= threshold;
    if (hit)
        *hit = found;
    return found ? line : value;
}

crop::Point snapPoint(const crop::Point &point, const Targets &targets) {
    return {snapCoordinate(point.x, targets.xs, targets.gridX, targets.threshold),
            snapCoordinate(point.y, targets.ys, targets.gridY, targets.threshold)};
}

crop::Rect snapRect(const crop::Rect &rect, const Targets &targets, const crop::Size &canvas, bool keepSize) {
    if (canvas.width <= 0 || canvas.height <= 0)
        return rect;
    const crop::Rect base = targets.multiple > 1 ? region::snapped(rect, targets.multiple, canvas, keepSize) : rect;
    crop::Rect out = base;
    if (targets.threshold > 0) {
        // Lines are measured from where the rectangle is (not from its multiple), so a nearby guide always wins.
        if (!targets.xs.empty() || targets.gridX > 0)
            pullAxis(rect.x, rect.width, base.x, base.width, canvas.width, targets.xs, targets.gridX, targets.threshold, keepSize, &out.x, &out.width);
        if (!targets.ys.empty() || targets.gridY > 0)
            pullAxis(rect.y, rect.height, base.y, base.height, canvas.height, targets.ys, targets.gridY, targets.threshold, keepSize, &out.y, &out.height);
    }
    return out;
}

} // namespace fugue::snap
