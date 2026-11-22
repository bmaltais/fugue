#pragma once

// What differs between two canvas revisions (#210, epic #206, spec 14.5): layers added, removed or
// changed, and for a changed one the tiles whose pixels differ and the rectangle they cover. Pure
// functions on two Trees (engine/canvas_store.h): no files, no pixels read. The command line prints
// it and the revision viewer (canvas/canvas_compare.h) draws the rectangles.

#include <QString>

#include <functional>
#include <vector>

#include "engine/canvas_store.h"

namespace fugue::canvas {

enum class LayerChange { Added, Removed, Changed };

struct LayerDiff {
    LayerChange change = LayerChange::Changed;
    QString name;
    int tilesChanged = 0;      // tiles that differ (for an added or removed layer, all of its tiles)
    mask::IRect bounds;        // the pixels those tiles cover (whole tiles); empty (0 x 0) when none. tightenBounds narrows it to the pixels that differ.
    std::vector<TileKey> changedTiles; // the tiles counted in tilesChanged
    int indexA = -1, indexB = -1;      // the layer's place in a and in b (-1: not there)
    QStringList properties;    // for a changed layer: "name" (a rename: same kind and pixels), "kind", "visible", "opacity", "blend", "mask"
};

struct TreeDiff {
    bool sizeChanged = false;
    int widthA = 0, heightA = 0, widthB = 0, heightB = 0;
    std::vector<LayerDiff> layers;     // in the order of b (added and changed), removed ones last
    bool identical() const { return !sizeChanged && layers.empty(); }
    mask::IRect bounds() const;        // the union of every layer's bounds
};

// The pixels of a stored tile, as CanvasStore::tilePixels gives them.
using TilePixels = std::function<Result<mask::MaskPaint>(const QString &hash, int width, int height, int channels)>;

// A diff counts tiles (a tile is the unit of storage, so a rectangle painted across a tile edge touches
// whole tiles). This narrows each layer's bounds, and the diff's, to the pixels that really differ,
// reading only the changed tiles. A layer whose tiles cannot be read, or whose canvas changed size,
// keeps its whole-tile bounds. Counts and the trees are untouched.
void tightenBounds(TreeDiff *diff, const Tree &a, const Tree &b, const TilePixels &read);

QString changeName(LayerChange change); // "added", "removed", "changed"

// Layers are paired by name, in order (the first "Fill" of a with the first "Fill" of b, and so on); a layer
// left over in b with the kind and pixels of one left over in a is that layer renamed.
TreeDiff diffTrees(const Tree &a, const Tree &b);

} // namespace fugue::canvas
