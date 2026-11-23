#include "engine/canvas_tree_resize.h"

#include <algorithm>

#include "engine/canvas_transform.h"
#include "engine/new_canvas.h"

namespace fugue::canvas {

namespace {

struct Move {
    int oldWidth, oldHeight, width, height, dx, dy;
    const TileReader &read;
    const TileSink &consume;
};

Result<mask::MaskPaint> assemble(const Move &move, int channels, const TileMap &tiles) {
    return assembleLayer(move.oldWidth, move.oldHeight, channels, tiles, [&](const TileKey &key, const QString &hash) {
        const mask::IRect rect = tileRect(key, move.oldWidth, move.oldHeight);
        return move.read(hash, rect.width, rect.height, channels);
    });
}

// The tiles of the layer (one or four channels) after the move; `outside` is what a new area holds.
Result<TileMap> movedTiles(const Move &move, int channels, const TileMap &tiles, uint8_t outside) {
    if (tiles.empty() && outside == 0)
        return TileMap{};
    const Result<mask::MaskPaint> pixels = assemble(move, channels, tiles);
    if (!pixels)
        return Result<TileMap>(pixels.errors());
    return cutLayer(mask::movedPixels(pixels.value(), move.width, move.height, move.dx, move.dy, outside), move.consume);
}

Result<LayerRecord> movedLayer(const Move &move, const LayerRecord &layer) {
    LayerRecord out = layer;
    if (layer.hasMask) {
        Result<TileMap> mask = movedTiles(move, 1, layer.maskTiles, 255); // a new area does not hide the layer
        if (!mask)
            return Result<LayerRecord>(mask.errors());
        out.maskTiles = std::move(mask.value());
    }
    if (layer.group) {
        for (LayerRecord &child : out.children) {
            Result<LayerRecord> next = movedLayer(move, child);
            if (!next)
                return next;
            child = std::move(next.value());
        }
    } else if (layer.kind != mask::LayerKind::Picture) { // a picture layer has no tiles: it is the node's input
        Result<TileMap> tiles = movedTiles(move, layer.channels(), layer.tiles, 0);
        if (!tiles)
            return Result<LayerRecord>(tiles.errors());
        out.tiles = std::move(tiles.value());
    }
    if (layer.source) {
        // A placed or transformed layer's original moves with it (#236): the placement shifts by the same corner, so a picture
        // that stuck out of the canvas shows its off-canvas part when the canvas grows. A layer painted on since (its original
        // is not current) has no use for it any more.
        if (hasCurrentOriginal(layer)) {
            out.source->transform.cx += move.dx;
            out.source->transform.cy += move.dy;
            out.source->rendered = renderedKey(out.tiles, out.hasMask, out.maskTiles);
        } else {
            out.source.reset();
        }
    }
    return out;
}

} // namespace

Result<Tree> resizedTree(const Tree &tree, const mask::IRect &to, edgefill::Mode fill, int blur, const TileReader &read, const TileSink &consume) {
    if (const QString why = mask::canvasSizeProblem(to.width, to.height); !why.isEmpty())
        return makeError(QStringLiteral("input_invalid"), why);
    if (to.x >= tree.width || to.y >= tree.height || to.x + to.width <= 0 || to.y + to.height <= 0)
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("The new area must hold part of the canvas (%1 x %2); it holds none.").arg(tree.width).arg(tree.height));
    const Move move{tree.width, tree.height, to.width, to.height, -to.x, -to.y, read, consume};
    Tree out;
    out.width = to.width;
    out.height = to.height;

    if (mask::growsCanvas(tree.width, tree.height, to) && fill != edgefill::Mode::Transparent) {
        // What the old canvas shows (a group as its children draw it), in the new canvas's pixels; the hole is everything the old canvas does not cover.
        const Result<mask::PaintDocument> shown = documentOf(tree, read);
        if (!shown)
            return Result<Tree>(shown.errors());
        const mask::MaskPaint flat = mask::movedPixels(shown->flatten(), to.width, to.height, move.dx, move.dy, 0);
        edgefill::Image image;
        image.width = to.width;
        image.height = to.height;
        image.channels = 4;
        image.data.resize(flat.pixels().size());
        for (size_t i = 0; i < image.data.size(); ++i)
            image.data[i] = float(flat.pixels()[i]) / 255.0f;
        std::vector<uint8_t> hole(size_t(to.width) * size_t(to.height), 1);
        for (int y = std::max(0, move.dy); y < std::min(to.height, move.dy + tree.height); ++y)
            for (int x = std::max(0, move.dx); x < std::min(to.width, move.dx + tree.width); ++x)
                hole[size_t(y) * size_t(to.width) + size_t(x)] = 0;
        edgefill::fillHole(image, hole, fill, blur);
        std::vector<uint8_t> bytes(image.data.size(), 0);
        for (size_t p = 0; p < hole.size(); ++p)
            if (hole[p])
                for (size_t c = 0; c < 4; ++c)
                    bytes[p * 4 + c] = uint8_t(std::clamp(std::lround(image.data[p * 4 + c] * 255.0f), 0L, 255L));
        Result<TileMap> tiles = cutLayer(mask::MaskPaint(to.width, to.height, bytes, 4), consume);
        if (!tiles)
            return Result<Tree>(tiles.errors());
        LayerRecord filler;
        filler.name = QString::fromLatin1(mask::resizeFillLayerName);
        filler.producer = QString::fromLatin1(mask::resizeProducer);
        filler.tiles = std::move(tiles.value());
        out.layers.push_back(std::move(filler));
    }
    for (const LayerRecord &layer : tree.layers) {
        Result<LayerRecord> next = movedLayer(move, layer);
        if (!next)
            return Result<Tree>(next.errors());
        out.layers.push_back(std::move(next.value()));
    }
    return out;
}

} // namespace fugue::canvas
