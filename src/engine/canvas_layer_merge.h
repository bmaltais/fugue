#pragma once

// The layer operations that touch pixels (#252, epic #206, spec 14.5): Merge down and Flatten. Each
// composites layers of a tree (engine/canvas_store.h) the way the picture shows them (PaintDocument::flatten:
// hidden layers and mask layers do not draw; opacity, blend and layer mask do; a group is its children's
// composite), cuts the result into tiles and gives the tree with the layers replaced. The tiles are read and
// written through two functions, so the module knows no folder: CanvasStore::editLayers passes its blobs.
// Pure otherwise; the tree it is given is not changed.

#include <functional>

#include "engine/canvas_layer_ops.h"

namespace fugue::canvas {

// The pixels of the stored tile with this hash (CanvasStore::tilePixels is one).
using TileReader = std::function<Result<mask::MaskPaint>(const QString &hash, int width, int height, int channels)>;

// MergeDown: the layer at `edit.layers` (one position, not the bottom one) is drawn into the layer under it.
//   The result takes the lower layer's place, name, blend and origin; the lower layer's opacity and layer mask
//   are baked into its pixels (so the merged layer has opacity 1 and no mask) and both layers' pixels are what
//   they drew. Both must be shown and be paint layers or groups: a hidden layer would vanish, a picture layer
//   has no pixels here, a mask layer is not drawn. input_invalid otherwise.
// Flatten: every shown paint layer and group becomes ONE paint layer named "Background" (opacity 1, normal, no
//   mask, no origin); hidden layers are dropped (as Photoshop does after asking); a mask layer stays, above it.
//   A tree that is one plain shown layer already is returned as it is. input_invalid with a picture layer.
// Any other op: input_invalid.
Result<Tree> applyPixelEdit(const Tree &tree, const LayerEdit &edit, const TileReader &read, const TileSink &consume);

} // namespace fugue::canvas
