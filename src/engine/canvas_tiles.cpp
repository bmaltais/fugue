#include "canvas_tiles.h"

#include <algorithm>

#include "engine/hash.h"

namespace fugue::canvas {

using mask::IRect;
using mask::MaskPaint;

namespace {

Error corrupt(const QString &what) {
    return makeError(QStringLiteral("canvas_corrupt"), QStringLiteral("The canvas store is damaged: %1.").arg(what));
}

} // namespace

bool TileKey::parse(const QString &text, TileKey *key) {
    const QStringList parts = text.split(QLatin1Char(','));
    if (parts.size() != 2)
        return false;
    int values[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        bool ok = false;
        if (parts[i].isEmpty() || parts[i].size() > 6 || !parts[i].at(0).isDigit())
            return false;
        values[i] = parts[i].toInt(&ok);
        if (!ok || QString::number(values[i]) != parts[i])
            return false;
    }
    key->x = values[0];
    key->y = values[1];
    return true;
}

int tilesAcross(int width) { return (width + tileSide - 1) / tileSide; }
int tilesDown(int height) { return (height + tileSide - 1) / tileSide; }

IRect tileRect(const TileKey &key, int width, int height) {
    const int x = key.x * tileSide, y = key.y * tileSide;
    return IRect{x, y, std::max(0, std::min(tileSide, width - x)), std::max(0, std::min(tileSide, height - y))};
}

QByteArray tileBytes(const MaskPaint &layer, const IRect &rect) {
    const int channels = layer.channels();
    const size_t rowBytes = size_t(rect.width) * size_t(channels);
    QByteArray bytes;
    bytes.reserve(qsizetype(rowBytes * size_t(rect.height)));
    const std::vector<uint8_t> &all = layer.pixels();
    for (int row = 0; row < rect.height; ++row) {
        const size_t from = (size_t(rect.y + row) * size_t(layer.width()) + size_t(rect.x)) * size_t(channels);
        bytes.append(reinterpret_cast<const char *>(all.data() + from), qsizetype(rowBytes));
    }
    return bytes;
}

QString tileHash(int width, int height, int channels, const QByteArray &bytes) {
    QByteArray header = QByteArrayLiteral("fugue-tile 1\n") + QByteArray::number(width) + 'x' + QByteArray::number(height) + 'x' +
                        QByteArray::number(channels) + '\n';
    return sha256(header + bytes);
}

bool allZero(const QByteArray &bytes) {
    return std::all_of(bytes.cbegin(), bytes.cend(), [](char c) { return c == 0; });
}

Result<QByteArray> tileToPng(const MaskPaint &tile) { return tile.toPng(tilePngLevel); }

Result<MaskPaint> tileFromPng(const QByteArray &png, int channels) {
    return channels == 1 ? MaskPaint::fromPng(png) : MaskPaint::fromPicture(png);
}

Result<TileMap> cutLayer(const MaskPaint &layer, const TileSink &consume) {
    TileMap tiles;
    const int channels = layer.channels();
    for (int ty = 0; ty < tilesDown(layer.height()); ++ty) {
        for (int tx = 0; tx < tilesAcross(layer.width()); ++tx) {
            const TileKey key{tx, ty};
            const IRect rect = tileRect(key, layer.width(), layer.height());
            const QByteArray bytes = tileBytes(layer, rect);
            if (allZero(bytes))
                continue;
            const QString hash = tileHash(rect.width, rect.height, channels, bytes);
            tiles.emplace(key, hash);
            if (consume) {
                const std::vector<uint8_t> raw(bytes.cbegin(), bytes.cend());
                const Errors problems = consume(key, hash, MaskPaint(rect.width, rect.height, raw, channels));
                if (hasErrors(problems))
                    return Result<TileMap>(problems);
            }
        }
    }
    return tiles;
}

Result<MaskPaint> assembleLayer(int width, int height, int channels, const TileMap &tiles, const TileSource &fetch) {
    std::vector<uint8_t> pixels(size_t(width) * size_t(height) * size_t(channels), 0);
    for (const auto &[key, hash] : tiles) {
        const IRect rect = tileRect(key, width, height);
        if (rect.empty())
            return corrupt(QStringLiteral("a tile (%1) lies outside its layer").arg(key.text()));
        const Result<MaskPaint> tile = fetch(key, hash);
        if (!tile)
            return Result<MaskPaint>(tile.errors());
        if (tile->width() != rect.width || tile->height() != rect.height || tile->channels() != channels)
            return corrupt(QStringLiteral("tile %1 is not the size or channels its layer needs").arg(key.text()));
        const std::vector<uint8_t> &source = tile->pixels();
        const size_t rowBytes = size_t(rect.width) * size_t(channels);
        for (int row = 0; row < rect.height; ++row) {
            const size_t to = (size_t(rect.y + row) * size_t(width) + size_t(rect.x)) * size_t(channels);
            std::copy_n(source.begin() + std::ptrdiff_t(size_t(row) * rowBytes), rowBytes, pixels.begin() + std::ptrdiff_t(to));
        }
    }
    return MaskPaint(width, height, pixels, channels);
}

} // namespace fugue::canvas
