// Tiles for the canvas store (#208, spec 14.5): a layer cut into 256 px squares named by the hash
// of their raw pixels, and put back together. Seam: engine/canvas_tiles.h, pure functions on MaskPaint.

#include <QtTest>

#include "engine/canvas_tiles.h"
#include "engine/hash.h"
#include "engine/mask_paint.h"

using namespace fugue;
using namespace fugue::canvas;
using namespace fugue::mask;

namespace {

MaskPaint patterned(int width, int height, int channels, int seed) {
    std::vector<uint8_t> pixels(size_t(width) * height * channels);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = uint8_t((i * 7 + size_t(seed) * 13 + i / 251) % 256);
    return MaskPaint(width, height, pixels, channels);
}

} // namespace

class TestCanvasTiles : public QObject {
    Q_OBJECT
private slots:
    void keysParseOnlyTheirOwnText() {
        TileKey key;
        QVERIFY(TileKey::parse("3,12", &key));
        QCOMPARE(key.x, 3);
        QCOMPARE(key.y, 12);
        QCOMPARE(key.text(), QString("3,12"));
        for (const char *bad : {"", "3", "3,", ",3", "-1,2", "01,2", "1,2,3", "a,b", "1 ,2", "1234567,1"})
            QVERIFY2(!TileKey::parse(QString::fromLatin1(bad), &key), bad);
    }

    void edgeTilesAreCutToThePicture() {
        QCOMPARE(tilesAcross(256), 1);
        QCOMPARE(tilesAcross(257), 2);
        const IRect corner = tileRect(TileKey{1, 1}, 300, 270);
        QCOMPARE(corner, (IRect{256, 256, 44, 14}));
        QVERIFY(tileRect(TileKey{2, 0}, 300, 270).empty());
    }

    void theHashCoversSizeAndChannelsNotOnlyBytes() {
        const QByteArray bytes(64, 'x');
        QVERIFY(isSha256(tileHash(4, 4, 4, bytes)));
        QCOMPARE(tileHash(4, 4, 4, bytes), tileHash(4, 4, 4, bytes));
        QVERIFY(tileHash(4, 4, 4, bytes) != tileHash(8, 2, 4, bytes));
        QVERIFY(tileHash(4, 4, 4, bytes) != tileHash(8, 8, 1, bytes));
    }

    void emptyTilesAreNotTiles() {
        MaskPaint layer(600, 300, 4);
        const Result<TileMap> tiles = cutLayer(layer);
        QVERIFY(tiles);
        QVERIFY(tiles->empty());
        layer.fillRect(300, 10, 2, 2, Mode::Paint, 1, Rgba{255, 0, 0, 255});
        const Result<TileMap> one = cutLayer(layer);
        QCOMPARE(one->size(), size_t(1));
        QVERIFY(one->count(TileKey{1, 0}));
    }

    void aLayerCutAndAssembledIsTheSameLayer() {
        for (const int channels : {1, 4}) {
            const MaskPaint layer = patterned(600, 300, channels, channels);
            std::map<QString, MaskPaint> kept;
            const Result<TileMap> tiles = cutLayer(layer, [&](const TileKey &, const QString &hash, const MaskPaint &tile) {
                const Result<QByteArray> png = tileToPng(tile);
                if (!png)
                    return png.errors();
                const Result<MaskPaint> back = tileFromPng(png.value(), channels);
                if (!back)
                    return back.errors();
                kept.insert_or_assign(hash, back.value());
                return Errors();
            });
            QVERIFY(tiles);
            QCOMPARE(tiles->size(), size_t(3 * 2));
            const Result<MaskPaint> assembled = assembleLayer(600, 300, channels, tiles.value(), [&](const TileKey &, const QString &hash) { return Result<MaskPaint>(kept.at(hash)); });
            QVERIFY(assembled);
            QCOMPARE(assembled->pixels(), layer.pixels());
        }
    }

    void aTileOfTheWrongSizeIsRefused() {
        TileMap tiles{{TileKey{0, 0}, "sha256:" + QString(64, 'a')}};
        const Result<MaskPaint> assembled = assembleLayer(300, 300, 4, tiles, [](const TileKey &, const QString &) { return Result<MaskPaint>(MaskPaint(10, 10, 4)); });
        QVERIFY(!assembled);
        QCOMPARE(assembled.error().code, QString("canvas_corrupt"));
    }
};

QTEST_GUILESS_MAIN(TestCanvasTiles)
#include "test_canvas_tiles.moc"
