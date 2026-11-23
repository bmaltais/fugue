#pragma once

// Placing a picture on a canvas (#236, epic #206, spec 14.5). The pure part of the feature: a source picture
// (engine/canvas_layer_source.h) and a placement (LayerTransform) give the canvas-sized pixels the layer shows. No files,
// no Qt GUI: CanvasStore::pasteLayer stores the result, and a test pins the numbers down apart from any window.

#include <QString>

#include "engine/canvas_layer_source.h"
#include "engine/mask_paint.h"
#include "engine/result.h"

namespace fugue::canvas {

// The largest picture that can be pasted or dropped on a canvas: a canvas's own limit (a side, and its area).
// The empty string when it fits, else the reason in plain words.
QString placeableProblem(int width, int height);

// A placement of an unscaled picture centered on (cx, cy), the pixels laid on whole pixels.
LayerTransform centeredPlacement(int sourceWidth, int sourceHeight, double cx, double cy);

// The canvas's pixels (`canvasWidth` x `canvasHeight`, straight RGBA, transparent where the picture is not) the source
// shows when placed by `transform`, a plain placement (no scale, turn or flip).
Result<mask::MaskPaint> renderPlaced(const mask::MaskPaint &source, const LayerTransform &transform, int canvasWidth, int canvasHeight);

} // namespace fugue::canvas
