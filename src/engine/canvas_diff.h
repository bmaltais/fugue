#pragma once

// What differs between two canvas revisions (#210, epic #206, spec 14.5): layers added, removed or
// changed, and for a changed one the tiles whose pixels differ and the rectangle they cover. Pure
// functions on two Trees (engine/canvas_store.h): no files, no pixels read. The command line prints
// it and the revision viewer (canvas/canvas_compare.h) draws the rectangles.

#include <QString>

#include <vector>

#include "engine/canvas_store.h"

namespace fugue::canvas {

enum class LayerChange { Added, Removed, Changed };

struct LayerDiff {
    LayerChange change = LayerChange::Changed;
    QString name;
    int tilesChanged = 0;      // tiles that differ (for an added or removed layer, all of its tiles)
    mask::IRect bounds;        // the pixels those tiles cover; empty (0 x 0) when none
    QStringList properties;    // for a changed layer: "visible", "opacity", "kind"
};

struct TreeDiff {
    bool sizeChanged = false;
    int widthA = 0, heightA = 0, widthB = 0, heightB = 0;
    std::vector<LayerDiff> layers;     // in the order of b (added and changed), removed ones last
    bool identical() const { return !sizeChanged && layers.empty(); }
    mask::IRect bounds() const;        // the union of every layer's bounds
};

QString changeName(LayerChange change); // "added", "removed", "changed"

// Layers are paired by name, in order (the first "Fill" of a with the first "Fill" of b, and so on).
TreeDiff diffTrees(const Tree &a, const Tree &b);

} // namespace fugue::canvas
