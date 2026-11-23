#pragma once

// The geometry of the Move and Free Transform gestures (#236, epic #216): what a drag of a handle, of the body or outside a corner
// does to a layer's placement (engine/canvas_layer_source.h LayerTransform). Plain structs and numbers (no Qt GUI, no store), so a
// test pins the behavior down apart from any window, as for the crop frame (engine/crop_geometry.h) and the snap function
// (canvas/snap.h). References: Photoshop's Free Transform (a corner or an edge scales, Shift keeps the proportions, Alt works from the
// center, outside a corner turns, Shift turns by 15 degrees) and Blender's transform gizmo.
//
// A drag is computed from the placement it started with and the two pointer points (where it was pressed, where it is now), never
// step by step, so going out and back returns to where the pointer is. Points are canvas pixels. A handle drag works in the box's
// own (turned) frame, so a turned layer scales along its own edges. The size never goes under one pixel and never flips (Layer >
// Transform flips); a transform is not clamped to the canvas, a layer may reach past it.

#include <vector>

#include "canvas/snap.h"
#include "engine/canvas_layer_source.h"
#include "engine/crop_geometry.h"

namespace fugue::xform {

enum class Handle { None, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left, Move, Rotate };
// "top-left", "top", "top-right", "right", "bottom-right", "bottom", "bottom-left", "left", "move", "rotate".
bool handleFromName(const QString &name, Handle *handle);

struct Keys {
    bool shift = false; // scale: keep the proportions; turn: steps of 15 degrees; move: along one axis
    bool alt = false;   // scale: from the center
};

// The placement after dragging `handle` from `from` to `to`.
canvas::LayerTransform dragged(const canvas::LayerTransform &start, Handle handle, const crop::Point &from, const crop::Point &to, Keys keys);

// The box's four corners as drawn (top left, top right, bottom right, bottom left), and the smallest upright rectangle around them.
std::vector<crop::Point> corners(const canvas::LayerTransform &placement);
crop::RectF bounds(const canvas::LayerTransform &placement);

// The placement pulled to the guides and grid of `targets` (canvas/snap.h): a move by the nearest of its bounds' edges and middle; a handle
// drag of an unturned box with no ratio or center held by the edge it moves. Anything else is left as dragged.
canvas::LayerTransform snapped(const canvas::LayerTransform &start, const canvas::LayerTransform &placement, Handle handle, Keys keys, const snap::Targets &targets);

} // namespace fugue::xform
