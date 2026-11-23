// Copy, Copy Merged and Copy selection as mask (#327): the pure functions of engine/canvas_copy.h, no window, no clipboard.

#include <QtTest>

#include "engine/canvas_copy.h"

using namespace fugue;
using namespace fugue::canvas;
using namespace fugue::mask;

namespace {

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

// A one-channel selection of the canvas's size: `value` inside the rectangle, 0 outside.
MaskPaint selected(int w, int h, IRect r, uint8_t value = 255) {
    std::vector<uint8_t> px(size_t(w) * size_t(h), 0);
    for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x)
            px[size_t(y) * size_t(w) + size_t(x)] = value;
    return MaskPaint(w, h, px, 1);
}

uint8_t alphaAt(const MaskPaint &m, int x, int y) { return m.pixels()[(size_t(y) * size_t(m.width()) + size_t(x)) * 4 + 3]; }

} // namespace

class TestCanvasCopy : public QObject {
    Q_OBJECT

private slots:
    void aSelectionIsCutToItsBoundsAndBecomesTheAlpha() {
        const MaskPaint source = solid(20, 10, {10, 20, 30, 200});
        std::vector<uint8_t> px = selected(20, 10, {4, 2, 6, 5}).pixels();
        px[size_t(2) * 20 + 4] = 0; // a corner of the block is not selected: the cut-out is not a rectangle
        px[size_t(4) * 20 + 9] = 128; // a soft edge
        const MaskPaint selection(20, 10, px, 1);
        const CopiedPicture copy = copyArea(source, {&selection, {}}).value();
        QCOMPARE(copy.bounds, (IRect{4, 2, 6, 5}));
        QCOMPARE(copy.pixels.width(), 6);
        QCOMPARE(copy.pixels.height(), 5);
        QCOMPARE(copy.pixels.pixels()[0], uint8_t(10)); // the colors stay straight
        QCOMPARE(alphaAt(copy.pixels, 0, 0), uint8_t(0));
        QCOMPARE(alphaAt(copy.pixels, 1, 0), uint8_t(200));
        QCOMPARE(alphaAt(copy.pixels, 5, 2), uint8_t(100)); // 200 x 128 / 255
        // The selection comes with the picture, cut the same way, for a Load Image's mask.
        QVERIFY(copy.selection);
        QCOMPARE(copy.selection->width(), 6);
        QCOMPARE(int(copy.selection->at(5, 2)), 128);
        QCOMPARE(int(copy.selection->at(0, 0)), 0);
        QVERIFY(!copyArea(source, {nullptr, {1, 1, 3, 3}}).value().selection); // a rectangle is only white: no mask
    }

    void aRectangleAndNoSelectionCopyWithoutChangingAlpha() {
        const MaskPaint source = solid(20, 10, {1, 2, 3, 77});
        const CopiedPicture part = copyArea(source, {nullptr, {15, 5, 10, 10}}).value(); // cut at the canvas's edge
        QCOMPARE(part.bounds, (IRect{15, 5, 5, 5}));
        QCOMPARE(alphaAt(part.pixels, 4, 4), uint8_t(77));
        const CopiedPicture whole = copyArea(source, {}).value(); // Photoshop: no selection copies the whole layer
        QCOMPARE(whole.bounds, (IRect{0, 0, 20, 10}));
    }

    void whatCannotBeCopiedIsAnError() {
        const MaskPaint source = solid(20, 10, {1, 2, 3, 255});
        const MaskPaint none(20, 10, 1);
        QVERIFY(!copyArea(source, {&none, {}}).ok());
        const MaskPaint wrongSize = selected(5, 5, {0, 0, 5, 5});
        QVERIFY(!copyArea(source, {&wrongSize, {}}).ok());
        QVERIFY(!copyArea(source, {nullptr, {30, 0, 5, 5}}).ok());
        QVERIFY(!areaAsMask(20, 10, {&none, {}}).ok());
    }

    void theMaskIsTheSelectionAtTheCanvasSize() {
        const MaskPaint selection = selected(20, 10, {4, 2, 6, 5}, 200);
        const MaskPaint mask = areaAsMask(20, 10, {&selection, {}}).value();
        QCOMPARE(mask.channels(), 1);
        QCOMPARE(mask.at(5, 3), uint8_t(200));
        QCOMPARE(mask.at(0, 0), uint8_t(0));
        const MaskPaint box = areaAsMask(20, 10, {nullptr, {2, 1, 3, 3}}).value();
        QCOMPARE(box.at(2, 1), uint8_t(255));
        QCOMPARE(box.at(5, 1), uint8_t(0));
        QCOMPARE(areaAsMask(20, 10, {}).value().at(19, 9), uint8_t(255));
    }

    void aLayerKeepsItsMaskButNotItsOpacity() {
        PaintDocument document(4, 4);
        const MaskPaint half = selected(4, 4, {0, 0, 4, 4}, 128);
        const MaskPaint pixels = solid(4, 4, {9, 9, 9, 255});
        LayerInfo info;
        info.name = "L";
        info.opacity = 0.5;
        info.hasMask = true;
        const int id = document.loadLayer(info, pixels, &half);
        QVERIFY(id != 0);
        const MaskPaint copy = layerPixels(document, 0).value();
        QCOMPARE(alphaAt(copy, 1, 1), uint8_t(128));
        QVERIFY(!layerPixels(document, 3).ok());
        document.setMaskEnabled(id, false);
        QCOMPARE(alphaAt(layerPixels(document, 0).value(), 1, 1), uint8_t(255));
    }
};

QTEST_GUILESS_MAIN(TestCanvasCopy)
#include "test_canvas_copy.moc"
