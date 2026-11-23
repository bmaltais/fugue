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
// Groups (#252) are layers that hold layers (LayerRecord::group, children): the positions here are the
// tree's top level; Group puts layers into a group, Ungroup takes a group's children out, and nothing
// else reaches inside one.

#include <QString>

#include <vector>

#include "engine/canvas_store.h"
#include "engine/result.h"

namespace fugue::canvas {

// SetMask (#230) gives one layer a layer mask, or a new one: the tiles of `maskTiles`, which the caller stored first
// (CanvasStore::setLayerMask cuts a painted mask and keeps its new tiles); the mask is on.
//
// #252: New is an empty transparent paint layer (`layers` empty: on top; one position: right above it; `text` its
// name, else "Layer N"). Group puts the chosen layers into one new group at the place of the highest of them
// (`text` its name, else "Group N"). Ungroup replaces each chosen group by its children. Properties applies the
// `steps` (each one of the edits above, on its own layers) as one edit: a dialog's name, opacity, blend and mask in
// one revision. MergeDown (the layer at the position goes into the one under it) and Flatten (every visible layer
// becomes one, `layers` empty) read and write pixels, so applyLayerEdit refuses them: applyPixelEdit
// (engine/canvas_layer_merge.h) does them, and CanvasStore::editLayers calls the right one.
enum class LayerOp { Visible, Opacity, Blend, Rename, Remove, Move, Duplicate, MaskEnabled, RemoveMask, SetMask, New, Group, Ungroup, MergeDown, Flatten, Properties };

struct LayerEdit {
    LayerOp op = LayerOp::Visible;
    std::vector<int> layers; // positions, bottom first; at least one, each once
    bool flag = false;       // Visible, MaskEnabled: the new value
    double number = 1;       // Opacity: 0 to 1
    QString text;            // Rename: the new name; Blend: a blend id (mask::blendId: "normal", "multiply", ...)
    int to = 0;              // Move: the position the lowest of the moved layers ends at (the others stay above it, in order)
    TileMap maskTiles;       // SetMask: the mask's one-channel tiles (absent tiles are 0, black: hidden)
    std::vector<LayerEdit> steps; // Properties: the edits it is made of, in order
};

// Whether an edit has to read and write pixels (MergeDown, Flatten) rather than only move layer records.
bool layerOpNeedsPixels(LayerOp op);

// The producer's suffix of an edit: "visible", "opacity", "blend", "rename", "delete", "move",
// "duplicate", "mask-enabled", "mask-delete", "mask-set", "new", "group", "ungroup", "merge-down", "flatten",
// "properties" (the window writes `canvas-view:<name>`).
QString layerOpName(LayerOp op);
// The op a name names (the command line's words, above); false for another.
bool layerOpFromName(const QString &name, LayerOp *op);

// The tree after the edit; input_invalid for a position that is not there, a name that is empty or
// too long, an opacity outside 0 to 1, a blend that is not one, a rename of more than one layer, or
// a mask edit of a layer that has no mask, or a SetMask of more than one layer or of a mask layer, a Group of
// a picture layer, an Ungroup of a layer that is no group. An edit that changes nothing gives the same tree.
Result<Tree> applyLayerEdit(const Tree &tree, const LayerEdit &edit);

// The name a duplicate of `name` gets: "<name> copy", then "<name> copy 2", ... not taken in `tree`.
QString duplicateName(const Tree &tree, const QString &name);
// "<stem> N" for the lowest N, counting from the number of layers there are, that no top-level layer of `tree` has ("Layer 3", "Group 1").
QString numberedName(const Tree &tree, const QString &stem);

} // namespace fugue::canvas
