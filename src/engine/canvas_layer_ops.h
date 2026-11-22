#pragma once

// Layer operations on a canvas revision (#226, epic #206, spec 14.5): hide, show, opacity, blend,
// rename, delete, reorder, duplicate, and a layer mask turned on, off or deleted. Each is a pure
// edit of a Tree (engine/canvas_store.h): the layer records change, the tiles are the same hashes,
// so no pixel is read, encoded or stored and the blobs are shared with the revision it came from.
// CanvasStore::editLayers stores the result as a NEW revision whose parent is the one edited; the
// window (canvas/canvas_layers.h) and `fugue canvas layer` both go through it.
//
// Layers are named by their position in the tree, bottom first (0 is the bottom), as the tree lists
// them. One edit can take several layers (a selection in the window): it is still one revision.
// Groups are later: they would add a "group" layer kind with children, and positions become paths;
// nothing here assumes a flat list beyond the position.

#include <QString>

#include <vector>

#include "engine/canvas_store.h"
#include "engine/result.h"

namespace fugue::canvas {

// SetMask (#230) gives one layer a layer mask, or a new one: the tiles of `maskTiles`, which the caller stored first
// (CanvasStore::setLayerMask cuts a painted mask and keeps its new tiles); the mask is on.
enum class LayerOp { Visible, Opacity, Blend, Rename, Remove, Move, Duplicate, MaskEnabled, RemoveMask, SetMask };

struct LayerEdit {
    LayerOp op = LayerOp::Visible;
    std::vector<int> layers; // positions, bottom first; at least one, each once
    bool flag = false;       // Visible, MaskEnabled: the new value
    double number = 1;       // Opacity: 0 to 1
    QString text;            // Rename: the new name; Blend: a blend id (mask::blendId: "normal", "multiply", ...)
    int to = 0;              // Move: the position the lowest of the moved layers ends at (the others stay above it, in order)
    TileMap maskTiles;       // SetMask: the mask's one-channel tiles (absent tiles are 0, black: hidden)
};

// The producer's suffix of an edit: "visible", "opacity", "blend", "rename", "delete", "move",
// "duplicate", "mask-enabled", "mask-delete", "mask-set" (the window writes `canvas-view:<name>`).
QString layerOpName(LayerOp op);
// The op a name names (the command line's words, above); false for another.
bool layerOpFromName(const QString &name, LayerOp *op);

// The tree after the edit; input_invalid for a position that is not there, a name that is empty or
// too long, an opacity outside 0 to 1, a blend that is not one, a rename of more than one layer, or
// a mask edit of a layer that has no mask, or a SetMask of more than one layer or of a mask layer. An edit that changes nothing gives the same tree.
Result<Tree> applyLayerEdit(const Tree &tree, const LayerEdit &edit);

// The name a duplicate of `name` gets: "<name> copy", then "<name> copy 2", ... not taken in `tree`.
QString duplicateName(const Tree &tree, const QString &name);

} // namespace fugue::canvas
