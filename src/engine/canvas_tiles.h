#pragma once

// Tiles for the canvas store (#208, epic #206, spec 14.5): a layer's pixels cut into 256 px squares,
// each named by the SHA-256 of its raw pixels, so the same pixels are the same name wherever they
// are and an edit changes only the tiles it touched. Pure functions on MaskPaint; no files, no
// Qt Quick. The store (engine/canvas_store.h) keeps the tiles' PNG blobs and the manifests.
//
// A tile's hash covers a small header (its width, height and channel count) and then its raw bytes,
// row after row, so a 1-channel tile can never equal a 4-channel one, nor an edge tile a full one.
// A tile whose bytes are all 0 (a mask's black, a color layer's transparent) is not a tile at all:
// a layer lists only its other tiles, and a missing one reads as 0.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>

#include "engine/mask_paint.h"
#include "engine/result.h"

namespace fugue::canvas {

inline constexpr int tileSide = 256;
// PNG level 1: about 3x faster to encode than the default 6, for a few percent more bytes.
inline constexpr int tilePngLevel = 1;

struct TileKey {
    int x = 0; // the tile's column and row: pixel (x * 256, y * 256) is its top left
    int y = 0;
    bool operator<(const TileKey &o) const { return y != o.y ? y < o.y : x < o.x; }
    bool operator==(const TileKey &o) const { return x == o.x && y == o.y; }
    QString text() const { return QStringLiteral("%1,%2").arg(x).arg(y); }
    static bool parse(const QString &text, TileKey *key); // "x,y", digits only
};

// One layer's non-empty tiles: key to hash, in key order.
using TileMap = std::map<TileKey, QString>;

// The rectangle of the picture a tile covers (cut to the picture's edge).
mask::IRect tileRect(const TileKey &key, int width, int height);
// How many tiles across and down a picture of this size has.
int tilesAcross(int width);
int tilesDown(int height);

// The raw pixels of the tile (the rectangle `rect` of the layer, row after row).
QByteArray tileBytes(const mask::MaskPaint &layer, const mask::IRect &rect);
// "sha256:<hex>" of the tile's header and bytes.
QString tileHash(int width, int height, int channels, const QByteArray &bytes);
bool allZero(const QByteArray &bytes);

// A layer cut into tiles: each non-empty tile's hash, and `consume` is told each one's key, hash
// and PNG blob (level 1) so the store can keep what it does not have. Hashing alone (no PNG) when
// `consume` is empty.
using TileSink = std::function<Errors(const TileKey &key, const QString &hash, const mask::MaskPaint &tile)>;
Result<TileMap> cutLayer(const mask::MaskPaint &layer, const TileSink &consume = {});

// A layer of `width` x `height` x `channels` made of tiles: `fetch` gives the pixels of the tile
// with a hash (the key says where it goes). Tiles not listed are 0. Refuses a tile that is not the
// size the layer's geometry gives it, or has the wrong channels.
using TileSource = std::function<Result<mask::MaskPaint>(const TileKey &key, const QString &hash)>;
Result<mask::MaskPaint> assembleLayer(int width, int height, int channels, const TileMap &tiles, const TileSource &fetch);

// A tile as a PNG blob (level 1) and back (the channels say how to read it).
Result<QByteArray> tileToPng(const mask::MaskPaint &tile);
Result<mask::MaskPaint> tileFromPng(const QByteArray &png, int channels);

} // namespace fugue::canvas
