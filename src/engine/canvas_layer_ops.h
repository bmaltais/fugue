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
// tree's top level, or, with `LayerEdit::parent` (#278), the children of one group; Group puts layers into a group,
// Ungroup takes a group's children out, and every other edit works on the layers of the level it is given.

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
enum class LayerOp { Visible, Opacity, Blend, Rename, Remove, Move, Duplicate, MaskEnabled, RemoveMask, SetMask, New, Group, Ungroup, MergeDown, Flatten, Properties, Lock };

struct LayerEdit {
    LayerOp op = LayerOp::Visible;
    std::vector<int> layers; // positions, bottom first; at least one, each once
    bool flag = false;       // Visible, MaskEnabled, Lock: the new value
    double number = 1;       // Opacity: 0 to 1
    QString text;            // Rename: the new name; Blend: a blend id (mask::blendId: "normal", "multiply", ...)
    int to = 0;              // Move: the position the lowest of the moved layers ends at (the others stay above it, in order)
    TileMap maskTiles;       // SetMask: the mask's one-channel tiles (absent tiles are 0, black: hidden)
    std::vector<LayerEdit> steps; // Properties: the edits it is made of, in order (each names its own `parent`)
    // The group the edit works inside (#278): the index of a group at the top level, then of a group in its children, and so on; empty is
    // the top level. `layers`, `to` and the new layer's place are then positions among that group's children (bottom first). A locked
    // group refuses what adds, removes, moves or masks its layers (Remove, Move, Duplicate, RemoveMask, SetMask, New, Group, Ungroup);
    // showing, opacity, blend, names and the mask switch stay open. MergeDown, Flatten and a Properties edit take no parent.
    std::vector<int> parent;
};

// The layer a name means (#314): Canvas Region's Layer and Canvas Write's Onto the pixels of layer resolve it the same way, so the
// two never name different layers. The top one (the last, bottom first) of that name that is not a picture layer; a group counts (Region
// reads it as the one picture it composites). `names` gets the layer names seen, for a hint. -1 when there is none.
struct LayerFace {
    QString name;
    mask::LayerKind kind = mask::LayerKind::Paint;
    bool group = false;
};
int topLayerNamed(const std::vector<LayerFace> &layers, const QString &name, QStringList *names = nullptr);
std::vector<LayerFace> layerFaces(const Tree &tree);
// Why a layer has no pixels to write (a group, a picture or a mask layer), in words; empty for a paint layer.
QString noPixelsReason(const LayerFace &layer);

// A locked layer (#226) refuses what changes its pixels or its mask, takes it out of the stack or merges it: Remove, RemoveMask, SetMask,
// Ungroup of a locked group, a transform (canvas_transform.h) and a merge. The error says which layer and how to unlock it.
Error lockedError(const LayerRecord &layer, const QString &what);

// Whether an edit has to read and write pixels (MergeDown, Flatten) rather than only move layer records.
bool layerOpNeedsPixels(LayerOp op);

// The producer's suffix of an edit: "visible", "opacity", "blend", "rename", "delete", "move",
// "duplicate", "mask-enabled", "mask-delete", "mask-set", "new", "group", "ungroup", "merge-down", "flatten",
// "properties", "lock" (the window writes `canvas-view:<name>`).
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
