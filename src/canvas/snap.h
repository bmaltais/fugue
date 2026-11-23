#pragma once

// The one snap function of the Canvas view (#253, epic #216): where a point or a rectangle on a canvas lands when it is
// pulled to the guides, the grid and the Region node's Multiple of. Plain structs and numbers (no Qt, no store, no
// picture), so the region's frame, the selection and a later Paste or Move (#236) all call the same code and a test
// pins it down apart from the window.
//
// Lines pull: a coordinate within `threshold` canvas pixels of a guide or a grid line goes onto it (the nearest wins; a
// guide wins a tie with the grid). The Multiple is the region's existing rule (region::snapped: every edge on the
// nearest multiple), which is applied first; a line that is close enough then wins over it, so a guide is always
// reachable. Each part is off when its input is empty or 0, so a caller turns "snap to guides", "snap to grid" and "snap
// to the multiple" on and off by what it puts in the targets.

#include <vector>

#include "engine/crop_geometry.h"

namespace fugue::snap {

struct Targets {
    std::vector<int> xs;       // vertical guides: canvas columns
    std::vector<int> ys;       // horizontal guides: canvas rows
    int gridX = 0;             // grid spacing across (a line every gridX columns from 0); 0: no grid
    int gridY = 0;             // grid spacing down; 0: no grid
    int multiple = 0;          // the Region node's Multiple of (<= 1: none)
    double threshold = 0;      // canvas pixels a line pulls from (<= 0: lines are off); the caller divides screen pixels by the zoom
    bool operator==(const Targets &o) const {
        return xs == o.xs && ys == o.ys && gridX == o.gridX && gridY == o.gridY && multiple == o.multiple && threshold == o.threshold;
    }
};

// One coordinate pulled to the nearest of `lines` and the grid (`spacing`, 0 for none) within `threshold`; `hit`, when
// given, says whether a line took it. The value comes back unchanged when none is near.
double snapCoordinate(double value, const std::vector<int> &lines, int spacing, double threshold, bool *hit = nullptr);

// A point: x against the columns, y against the rows. The multiple does not apply to a lone point.
crop::Point snapPoint(const crop::Point &point, const Targets &targets);

// A rectangle inside `canvas`. With `keepSize` (a move, a ratio lock) the size stays and the rectangle shifts by the
// smaller pull of its two edges per axis; otherwise each edge is pulled on its own (never to less than one pixel wide).
crop::Rect snapRect(const crop::Rect &rect, const Targets &targets, const crop::Size &canvas, bool keepSize = false);

} // namespace fugue::snap
