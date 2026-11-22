#include "canvas_diff.h"

#include <algorithm>
#include <map>
#include <set>

namespace fugue::canvas {

namespace {

// The tiles of `a` and `b` (key to hash, absent = empty) that are not the same, as a count and the rectangle they cover.
void tileDifference(const TileMap &a, const TileMap &b, int width, int height, LayerDiff *out) {
    std::set<TileKey> keys;
    for (const auto &entry : a)
        keys.insert(entry.first);
    for (const auto &entry : b)
        keys.insert(entry.first);
    for (const TileKey &key : keys) {
        const auto inA = a.find(key), inB = b.find(key);
        if (inA != a.end() && inB != b.end() && inA->second == inB->second)
            continue;
        ++out->tilesChanged;
        out->bounds = out->bounds.united(tileRect(key, width, height));
    }
}

} // namespace

QString changeName(LayerChange change) {
    return change == LayerChange::Added ? QStringLiteral("added") : change == LayerChange::Removed ? QStringLiteral("removed") : QStringLiteral("changed");
}

mask::IRect TreeDiff::bounds() const {
    mask::IRect all;
    for (const LayerDiff &layer : layers)
        all = all.united(layer.bounds);
    return all;
}

TreeDiff diffTrees(const Tree &a, const Tree &b) {
    TreeDiff diff;
    diff.widthA = a.width;
    diff.heightA = a.height;
    diff.widthB = b.width;
    diff.heightB = b.height;
    diff.sizeChanged = a.width != b.width || a.height != b.height;

    // Pair by name, in order.
    std::map<QString, std::vector<int>> unpaired;
    for (int i = 0; i < int(a.layers.size()); ++i)
        unpaired[a.layers[i].name].push_back(i);
    std::vector<bool> used(a.layers.size(), false);
    for (const LayerRecord &later : b.layers) {
        LayerDiff entry;
        entry.name = later.name;
        auto found = unpaired.find(later.name);
        if (found == unpaired.end() || found->second.empty()) {
            entry.change = LayerChange::Added;
            tileDifference({}, later.tiles, b.width, b.height, &entry);
            diff.layers.push_back(entry);
            continue;
        }
        const int index = found->second.front();
        found->second.erase(found->second.begin());
        used[index] = true;
        const LayerRecord &earlier = a.layers[index];
        if (earlier.kind != later.kind)
            entry.properties << QStringLiteral("kind");
        if (earlier.visible != later.visible)
            entry.properties << QStringLiteral("visible");
        if (earlier.opacity != later.opacity)
            entry.properties << QStringLiteral("opacity");
        tileDifference(earlier.tiles, later.tiles, std::max(a.width, b.width), std::max(a.height, b.height), &entry);
        if (entry.tilesChanged > 0 || !entry.properties.isEmpty())
            diff.layers.push_back(entry);
    }
    for (int i = 0; i < int(a.layers.size()); ++i) {
        if (used[i])
            continue;
        LayerDiff entry;
        entry.change = LayerChange::Removed;
        entry.name = a.layers[i].name;
        tileDifference(a.layers[i].tiles, {}, a.width, a.height, &entry);
        diff.layers.push_back(entry);
    }
    return diff;
}

} // namespace fugue::canvas
