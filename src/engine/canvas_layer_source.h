#pragma once

// The original pixels of a placed or transformed layer (#236, epic #206, spec 14.5): what a Smart Object keeps in
// Photoshop, in its lightest form. A picture pasted on a canvas, or a layer a transform has changed, keeps the pixels it
// started from (`LayerSource`), where the layer's own tiles hold what the canvas shows: the picture placed by its
// `LayerTransform`, cut at the canvas's edge. A later transform starts again from the source, so scaling a layer down and
// back up loses nothing, and a picture bigger than the canvas keeps its off-canvas part.
//
// The layer's tiles stay the picture a revision shows: every reader (the composite, a node, .ora, .psd, a flattened
// export) reads the tiles and knows nothing of the source. The source is stored in the tree (`"source"` in a layer's
// record, written only for a layer that has one, so a tree from before keeps its hash) and its tiles are blobs of the
// store like any other, shared by content. `rendered` ties the two together: it is the key (renderedKey) of the layer's
// tiles and mask the transform produced. A layer painted on, or given another mask, since then has other tiles: the
// key no longer matches, the source is stale, and the next transform starts from the layer as it is now (the same
// rule as rasterising a Smart Object when you paint on it).
//
// Plain data and JSON, no Qt GUI: the store reads and writes it and the package and export code walk its tiles.

#include <QJsonObject>
#include <QString>

#include "engine/canvas_tiles.h"
#include "engine/result.h"

namespace fugue::canvas {

// How a picture is resampled when it is scaled or turned.
enum class ResampleFilter { Lanczos, Bicubic, Nearest };
QString filterName(ResampleFilter filter); // "lanczos", "bicubic", "nearest"
bool filterFromName(const QString &name, ResampleFilter *filter);

// Where the source lands on the canvas. The source is first flipped, scaled to `width` x `height` (canvas pixels, before
// the turn), then turned by `angle` degrees clockwise about its own center, which sits at (`cx`, `cy`) on the canvas.
// Numbers are kept to a thousandth of a pixel or degree (normalized), so the same placement is the same bytes.
struct LayerTransform {
    double cx = 0;
    double cy = 0;
    double width = 0;
    double height = 0;
    double angle = 0; // degrees, clockwise, in (-180, 180]
    bool flipX = false;
    bool flipY = false;
    ResampleFilter filter = ResampleFilter::Lanczos;
    bool operator==(const LayerTransform &o) const {
        return cx == o.cx && cy == o.cy && width == o.width && height == o.height && angle == o.angle && flipX == o.flipX && flipY == o.flipY && filter == o.filter;
    }
    LayerTransform normalized() const; // rounded, the angle brought into (-180, 180]
    // Whether it is a plain placement: no turn, no flip, and the source's own size (a copy of pixels, nothing resampled).
    bool isPlacement(int sourceWidth, int sourceHeight) const;
};

// The largest a source is (a canvas's own limit) and the largest a source is scaled to (a side, and its area).
inline constexpr int maxTransformSide = 32768;
inline constexpr double maxTransformArea = 268435456.0; // 16384 x 16384

// The placement a caller worked from when it asked for a transform (CanvasStore::transformLayer): the store refuses the transform when the layer is
// no longer that, so a placement read from one revision is never applied to another.
struct SeenPlacement {
    LayerTransform transform;
    bool original = false; // the layer had a current original (else it was its own pixels)
    int sourceWidth = 0;
    int sourceHeight = 0;
};

struct LayerSource {
    int width = 0;
    int height = 0;
    TileMap tiles;     // four channels, `width` x `height`
    bool hasMask = false;
    TileMap maskTiles; // the layer mask as the source had it, one channel, `width` x `height`
    LayerTransform transform;
    QString rendered;  // renderedKey of the layer's tiles and mask that this transform produced
    bool operator==(const LayerSource &o) const {
        return width == o.width && height == o.height && tiles == o.tiles && hasMask == o.hasMask && maskTiles == o.maskTiles && transform == o.transform &&
               rendered == o.rendered;
    }
    bool operator!=(const LayerSource &o) const { return !(*this == o); }
    int tileCount() const { return int(tiles.size() + maskTiles.size()); }
};

// The key that says which pixels a layer shows: the canonical hash of its tiles and, when it has one, its mask's.
QString renderedKey(const TileMap &tiles, bool hasMask, const TileMap &maskTiles);

QJsonObject sourceToJson(const LayerSource &source);
// A bad one is canvas_corrupt: sizes, tile keys, the transform's numbers and the filter are all checked (the file may be someone else's).
Result<LayerSource> sourceFromJson(const QJsonObject &object);

} // namespace fugue::canvas
