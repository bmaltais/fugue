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
        out->changedTiles.push_back(key);
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
            entry.indexB = int(&later - b.layers.data());
            tileDifference({}, later.tiles, b.width, b.height, &entry);
            diff.layers.push_back(entry);
            continue;
        }
        const int index = found->second.front();
        found->second.erase(found->second.begin());
        used[index] = true;
        entry.indexA = index;
        entry.indexB = int(&later - b.layers.data());
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
        entry.indexA = i;
        tileDifference(a.layers[i].tiles, {}, a.width, a.height, &entry);
        diff.layers.push_back(entry);
    }
    return diff;
}

namespace {

// The rectangle of pixels of tile `key` (in layer coordinates) where the two layers differ; a missing
// side reads as 0. Empty when the tiles cannot be compared or read.
bool differingPixels(const LayerRecord *la, const Tree &a, const LayerRecord *lb, const Tree &b, const TileKey &key, const TilePixels &read,
                     mask::IRect *out) {
    const LayerRecord &ref = la ? *la : *lb;
    const int channels = ref.channels();
    const mask::IRect rect = tileRect(key, b.width, b.height);
    if (a.width != b.width || a.height != b.height || rect.empty())
        return false;
    auto load = [&](const LayerRecord *layer, std::vector<uint8_t> *bytes) {
        bytes->assign(size_t(rect.width) * rect.height * channels, 0);
        if (!layer)
            return true;
        const auto found = layer->tiles.find(key);
        if (found == layer->tiles.end())
            return true;
        const Result<mask::MaskPaint> tile = read(found->second, rect.width, rect.height, channels);
        if (!tile || tile->width() != rect.width || tile->height() != rect.height)
            return false;
        *bytes = tile->pixels();
        return bytes->size() == size_t(rect.width) * rect.height * channels;
    };
    std::vector<uint8_t> pa, pb;
    if (!load(la, &pa) || !load(lb, &pb))
        return false;
    int x0 = rect.width, y0 = rect.height, x1 = -1, y1 = -1;
    for (int y = 0; y < rect.height; ++y) {
        for (int x = 0; x < rect.width; ++x) {
            const size_t at = (size_t(y) * rect.width + x) * channels;
            if (std::equal(pa.begin() + at, pa.begin() + at + channels, pb.begin() + at))
                continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    *out = x1 < 0 ? mask::IRect{} : mask::IRect{rect.x + x0, rect.y + y0, x1 - x0 + 1, y1 - y0 + 1};
    return true;
}

} // namespace

void tightenBounds(TreeDiff *diff, const Tree &a, const Tree &b, const TilePixels &read) {
    for (LayerDiff &layer : diff->layers) {
        if (layer.tilesChanged == 0)
            continue;
        const LayerRecord *la = layer.indexA >= 0 ? &a.layers[layer.indexA] : nullptr;
        const LayerRecord *lb = layer.indexB >= 0 ? &b.layers[layer.indexB] : nullptr;
        mask::IRect tight;
        bool ok = true;
        for (const TileKey &key : layer.changedTiles) {
            mask::IRect part;
            if (!differingPixels(la, a, lb, b, key, read, &part)) {
                ok = false;
                break;
            }
            tight = tight.united(part);
        }
        if (ok)
            layer.bounds = tight;
    }
}

} // namespace fugue::canvas
