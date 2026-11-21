#pragma once

// Where a floating bar sits inside an area (#120): kept inside the area, remembered as a fraction of
// the room it has (so a resized area keeps it where it was, relative to the edges), pulled lightly to
// the edges and the middle, and the side a panel hung from it opens on. Plain numbers: it knows
// nothing of Qt Quick, the canvas or the run toolbar, so any floating bar or palette can use it
// (src/canvas/float_place_qml.h gives it to QML; src/canvas/MoveGrip.qml is the drag and the keys).
//
// The box (the bar) keeps `margin` from each edge of the area. The room it moves in is then
// [margin, area - box - margin] on each axis, and a fraction is a place in that room: 0 at the start
// edge, 0.5 in the middle, 1 at the far edge. When the room is empty or negative (a box wider than
// the area) the box is centered and the fraction reads 0.5.

#include <algorithm>
#include <cmath>

namespace fugue::place {

struct Point {
    double x = 0;
    double y = 0;
    bool operator==(const Point &o) const { return x == o.x && y == o.y; }
};

struct Size {
    double width = 0;
    double height = 0;
};

struct Rect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

// A place as fractions of the room (0 to 1 on each axis).
struct Frac {
    double x = 0.5;
    double y = 0;
    bool operator==(const Frac &o) const { return x == o.x && y == o.y; }
};

struct Bounds {
    double width = 0;  // the area
    double height = 0;
    double boxWidth = 0; // the box that moves in it
    double boxHeight = 0;
    double margin = 0;
};

namespace detail {

struct Span {
    double lo;
    double hi;
};

inline Span span(double area, double box, double margin) {
    const double lo = margin;
    const double hi = area - box - margin;
    if (hi < lo)
        return {(area - box) / 2, (area - box) / 2};
    return {lo, hi};
}

inline double clampTo(double v, Span s) { return std::clamp(v, s.lo, s.hi); }

inline double fraction(double v, Span s) {
    if (s.hi <= s.lo)
        return 0.5;
    return std::clamp((v - s.lo) / (s.hi - s.lo), 0.0, 1.0);
}

inline double pull(double v, Span s, double threshold) {
    const double stops[3] = {s.lo, (s.lo + s.hi) / 2, s.hi};
    double best = v;
    double bestDistance = threshold;
    for (const double stop : stops) {
        const double d = std::abs(v - stop);
        if (d <= bestDistance) {
            best = stop;
            bestDistance = d;
        }
    }
    return best;
}

} // namespace detail

inline Point clamped(Point p, const Bounds &b) {
    return {detail::clampTo(p.x, detail::span(b.width, b.boxWidth, b.margin)),
            detail::clampTo(p.y, detail::span(b.height, b.boxHeight, b.margin))};
}

inline Point fromFraction(Frac f, const Bounds &b) {
    const detail::Span sx = detail::span(b.width, b.boxWidth, b.margin);
    const detail::Span sy = detail::span(b.height, b.boxHeight, b.margin);
    return {sx.lo + std::clamp(f.x, 0.0, 1.0) * (sx.hi - sx.lo), sy.lo + std::clamp(f.y, 0.0, 1.0) * (sy.hi - sy.lo)};
}

inline Frac toFraction(Point p, const Bounds &b) {
    return {detail::fraction(p.x, detail::span(b.width, b.boxWidth, b.margin)),
            detail::fraction(p.y, detail::span(b.height, b.boxHeight, b.margin))};
}

// Pulled to the start edge, the middle or the far edge of each axis that lies within `threshold` of
// it, each axis on its own. The nearest wins; a threshold of 0 or less leaves `p` alone.
inline Point snapped(Point p, const Bounds &b, double threshold) {
    if (threshold <= 0)
        return p;
    return {detail::pull(p.x, detail::span(b.width, b.boxWidth, b.margin), threshold),
            detail::pull(p.y, detail::span(b.height, b.boxHeight, b.margin), threshold)};
}

// `p` moved by a step and held inside.
inline Point moved(Point p, double dx, double dy, const Bounds &b) { return clamped({p.x + dx, p.y + dy}, b); }

// Room for a panel under and over the bar, `gap` apart from it.
inline double roomBelow(Rect bar, Size area, double gap) { return std::max(0.0, area.height - (bar.y + bar.height) - gap); }
inline double roomAbove(Rect bar, double gap) { return std::max(0.0, bar.y - gap); }

// Whether a panel `panelHeight` tall hangs under the bar: when it fits there, else when there is
// more room under than over.
inline bool opensBelow(Rect bar, double panelHeight, Size area, double gap) {
    const double below = roomBelow(bar, area, gap);
    return below >= panelHeight || below >= roomAbove(bar, gap);
}

// The top left of a panel hung from the bar on the side with room, centered on the bar and held
// inside the area sideways. The caller limits the panel's height to the room on that side. The side
// is chosen for `sideHeight` (the height the panel wants, when it is kept shorter than that to fit),
// or for the panel's own height when that is negative.
inline Point panelPlace(Rect bar, Size panel, Size area, double gap, double sideHeight = -1) {
    const double x = std::clamp(bar.x + (bar.width - panel.width) / 2, 0.0, std::max(0.0, area.width - panel.width));
    const double y = opensBelow(bar, sideHeight < 0 ? panel.height : sideHeight, area, gap) ? bar.y + bar.height + gap : std::max(0.0, bar.y - gap - panel.height);
    return {x, y};
}

} // namespace fugue::place
