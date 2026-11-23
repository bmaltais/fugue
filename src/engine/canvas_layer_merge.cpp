#include "engine/canvas_layer_merge.h"

#include <algorithm>

namespace fugue::canvas {

namespace {

Error invalid(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("input_invalid"), message, hint); }

// The picture the layers of `layers` draw together, as one tile-cut layer record's pixels.
Result<mask::MaskPaint> composite(const Tree &tree, std::vector<LayerRecord> layers, const TileReader &read) {
    Tree inner;
    inner.width = tree.width;
    inner.height = tree.height;
    inner.layers = std::move(layers);
    const Result<mask::PaintDocument> document = documentOf(inner, read);
    if (!document)
        return Result<mask::MaskPaint>(document.errors());
    return document->flatten();
}

Result<TileMap> cut(const mask::MaskPaint &pixels, const TileSink &consume) { return cutLayer(pixels, consume); }

bool isPlain(const LayerRecord &layer) {
    return !layer.group && layer.kind == mask::LayerKind::Paint && layer.visible && layer.opacity == 1 && layer.compositeOp.isEmpty() && !layer.hasMask;
}

Result<Tree> mergeDown(const Tree &tree, int upper, const TileReader &read, const TileSink &consume) {
    const int count = int(tree.layers.size());
    if (upper < 1 || upper >= count)
        return invalid(upper == 0 ? QStringLiteral("The bottom layer has no layer under it to merge into.") : QStringLiteral("There is no layer %1: the revision has %2.").arg(upper).arg(count));
    const LayerRecord &top = tree.layers[size_t(upper)];
    const LayerRecord &bottom = tree.layers[size_t(upper) - 1];
    for (const LayerRecord *layer : {&bottom, &top}) {
        if (layer->kind != mask::LayerKind::Paint)
            return invalid(QStringLiteral("%1 is a %2 layer: only paint layers and groups merge.").arg(layer->name, layer->kind == mask::LayerKind::Mask ? QStringLiteral("mask") : QStringLiteral("picture")));
        if (layer->locked)
            return lockedError(*layer, QStringLiteral("it cannot be merged"));
        if (!layer->visible)
            return invalid(QStringLiteral("%1 is hidden: show it, or delete it, before merging.").arg(layer->name));
        if (layer->hasMask && !layer->maskEnabled)
            return invalid(QStringLiteral("%1 has a layer mask that is turned off: merging would lose it.").arg(layer->name),
                           QStringLiteral("Turn the mask on (it is then baked in) or delete it, then merge."));
    }
    LayerRecord drawn = bottom; // what the lower layer draws: opacity and mask baked in, no blend of its own against nothing
    drawn.compositeOp.clear();
    const Result<mask::MaskPaint> pixels = composite(tree, {drawn, top}, read);
    if (!pixels)
        return Result<Tree>(pixels.errors());
    Result<TileMap> tiles = cut(pixels.value(), consume);
    if (!tiles)
        return Result<Tree>(tiles.errors());
    LayerRecord merged;
    merged.name = bottom.name;
    merged.compositeOp = bottom.compositeOp;
    merged.producer = bottom.producer;
    merged.created = bottom.created;
    merged.tiles = std::move(tiles.value());
    Tree out = tree;
    out.layers.erase(out.layers.begin() + upper);
    out.layers[size_t(upper) - 1] = std::move(merged);
    return out;
}

Result<Tree> flatten(const Tree &tree, const TileReader &read, const TileSink &consume) {
    for (const LayerRecord &layer : tree.layers)
        if (layer.locked)
            return lockedError(layer, QStringLiteral("it cannot be flattened"));
    for (const LayerRecord &layer : tree.layers)
        if (layer.kind == mask::LayerKind::Picture)
            return invalid(QStringLiteral("%1 is a picture layer: a picture is a node's input and cannot be flattened into the canvas.").arg(layer.name));
    if (tree.layers.size() == 1 && isPlain(tree.layers.front()))
        return tree;
    std::vector<LayerRecord> drawing, masks;
    for (const LayerRecord &layer : tree.layers)
        (layer.kind == mask::LayerKind::Mask ? masks : drawing).push_back(layer);
    const Result<mask::MaskPaint> pixels = composite(tree, drawing, read);
    if (!pixels)
        return Result<Tree>(pixels.errors());
    Result<TileMap> tiles = cut(pixels.value(), consume);
    if (!tiles)
        return Result<Tree>(tiles.errors());
    LayerRecord flat;
    flat.name = QStringLiteral("Background");
    flat.tiles = std::move(tiles.value());
    Tree out = tree;
    out.layers.clear();
    out.layers.push_back(std::move(flat));
    out.layers.insert(out.layers.end(), masks.begin(), masks.end());
    return out;
}

} // namespace

Result<Tree> applyPixelEdit(const Tree &tree, const LayerEdit &edit, const TileReader &read, const TileSink &consume) {
    if (edit.op == LayerOp::MergeDown) {
        if (edit.layers.size() != 1)
            return invalid(QStringLiteral("Merge down takes one layer."));
        return mergeDown(tree, edit.layers.front(), read, consume);
    }
    if (edit.op == LayerOp::Flatten) {
        if (!edit.layers.empty())
            return invalid(QStringLiteral("Flatten takes the whole canvas, no layer."));
        return flatten(tree, read, consume);
    }
    return invalid(QStringLiteral("%1 does not read pixels.").arg(layerOpName(edit.op)));
}

} // namespace fugue::canvas
