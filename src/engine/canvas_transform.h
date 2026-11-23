#pragma once

// Placing and transforming a layer's original pixels on a canvas (#236, epic #206, spec 14.5). The pure part of the
// feature: a source picture (engine/canvas_layer_source.h) and a placement (LayerTransform) give the canvas-sized
// pixels the layer shows. No files, no Qt GUI: CanvasStore::pasteLayer and transformLayer store the result, and a test
// pins the numbers down apart from any window.
//
// A source is straight (not premultiplied) 8-bit RGBA. It is resampled on premultiplied alpha, so what is transparent
// leaks no color into its neighbours. The resampling is two steps, the way Photoshop's is two operations:
//   1. flip and scale to W x H = (round(width), round(height)): separable, each axis with the filter (Lanczos a = 3 or
//      Catmull-Rom bicubic, their support widened when shrinking, as engine/mask_paint.h's `resampled` and the Resize
//      nodes do; nearest picks the pixel whose center the new pixel's center falls in);
//   2. turn by `angle` about the center, when it is not 0 or a multiple of 90 degrees: the same filter at the turned
//      positions (a multiple of 90 lands exactly on pixels, so the turn loses nothing), with a one pixel soft edge.
// Only the part of the scaled picture the canvas shows is computed, so a picture far bigger than the canvas costs the
// canvas's area, not its own. The picture outside the canvas is cut here and kept whole in the source.

#include <QString>

#include "engine/canvas_layer_merge.h"
#include "engine/canvas_layer_source.h"
#include "engine/mask_paint.h"
#include "engine/result.h"

namespace fugue::canvas {

// The largest picture that can be pasted or dropped on a canvas: a canvas's own limit (a side, and its area).
// The empty string when it fits, else the reason in plain words.
QString placeableProblem(int width, int height);

// A placement of an unscaled picture centered on (cx, cy), the pixels laid on whole pixels.
LayerTransform centeredPlacement(int sourceWidth, int sourceHeight, double cx, double cy);
// The same placement with the turned box's corner on a whole pixel, for an angle that is a multiple of 90 (the others are left).
LayerTransform snappedToPixels(const LayerTransform &transform);
// Mirrored on screen about the box's center: the flip is toggled and the turn reversed (a mirror turns the other way).
LayerTransform flippedOnScreen(const LayerTransform &transform, bool horizontal);
// Turned a quarter turn about the center (clockwise or not), the corner on a whole pixel.
LayerTransform turnedQuarter(const LayerTransform &transform, bool clockwise);
// Whether a placement can be drawn: finite numbers, a size of at least one pixel and no more than maxTransformSide
// and maxTransformArea. Empty when it can, else the reason in plain words.
QString transformProblem(const LayerTransform &transform);

// The canvas's pixels (`canvasWidth` x `canvasHeight`, straight RGBA, transparent where the picture is not) the source
// shows when placed by `transform`. input_invalid for a placement transformProblem refuses.
Result<mask::MaskPaint> renderPlaced(const mask::MaskPaint &source, const LayerTransform &transform, int canvasWidth, int canvasHeight);
// The same for a one-channel mask that travels with the layer: where the picture is not, the mask is 255 (it hides nothing).
Result<mask::MaskPaint> renderPlacedMask(const mask::MaskPaint &source, const LayerTransform &transform, int canvasWidth, int canvasHeight);

// What a transform starts from for the layer at one place of a tree.
struct CurrentPlacement {
    LayerTransform transform; // where the original is now (a layer with none: where its pixels are, see below)
    int sourceWidth = 0;
    int sourceHeight = 0;
    bool original = false;    // the layer has an original that is still what it shows (else the next transform starts from its pixels)
};
// A layer's original is current while its tiles and mask are the ones the placement made (renderedKey).
bool hasCurrentOriginal(const LayerRecord &layer);
// A layer with no current original starts from the pixels it shows: the smallest rectangle that holds every pixel that is not
// transparent (Photoshop's Free Transform box), which becomes its original at the identity placement; its mask is cut to the same
// rectangle. input_invalid, in words, for a layer with no such pixel. `consume` is told the tiles of the new original (empty: not stored).
Result<LayerSource> originalOf(const LayerRecord &layer, int canvasWidth, int canvasHeight, const TileReader &read, const TileSink &consume = {});
Result<CurrentPlacement> currentPlacement(const LayerRecord &layer, int canvasWidth, int canvasHeight, const TileReader &read);

// The tree with the top-level layer at `position` placed by `transform`: it is drawn again from the original (the layer's own pixels
// the first time, which then become its original), its mask with it, and the original and the new placement are kept. The tiles are
// read and written through `read` and `consume`. input_invalid, in plain words, for a position that is not there, a group, a mask
// or picture layer, or a placement that cannot be drawn. A placement equal to the current one gives the tree as it is.
Result<Tree> transformedTree(const Tree &tree, int position, const LayerTransform &transform, const TileReader &read, const TileSink &consume);

} // namespace fugue::canvas
