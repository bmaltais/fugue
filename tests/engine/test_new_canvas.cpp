// The documents a new canvas starts from (#232, epic #206, spec 14.5): a blank one of a size and a background, or
// one from pixels, under the size caps `fugue canvas new` and the New canvas dialog share. Seam: engine/new_canvas.h.

#include <QtTest>

#include "engine/new_canvas.h"

using namespace fugue;
using namespace fugue::mask;

class TestNewCanvas : public QObject {
    Q_OBJECT

    static Rgba pixel(const PaintDocument &document, int x, int y) { return document.pixels(document.layer(0).id)->rgbaAt(x, y); }

private slots:
    void backgroundsAreWordsOrHexColors() {
        QVERIFY(!parseBackground("transparent")->filled);
        QVERIFY(!parseBackground("")->filled);
        QCOMPARE(parseBackground("White")->color, (Rgba{255, 255, 255, 255}));
        QCOMPARE(parseBackground(" black ")->color, (Rgba{0, 0, 0, 255}));
        QCOMPARE(parseBackground("#336699")->color, (Rgba{0x33, 0x66, 0x99, 255}));
        QCOMPARE(parseBackground("F80")->color, (Rgba{0xff, 0x88, 0x00, 255}));
        const Result<CanvasBackground> bad = parseBackground("teal-ish");
        QVERIFY(!bad);
        QVERIFY(bad.error().message.contains("transparent, white, black or a color"));
    }

    void sizesAreHeldToTheCapsInPlainWords() {
        QVERIFY(canvasSizeProblem(1, 1).isEmpty());
        QVERIFY(canvasSizeProblem(16384, 512).isEmpty());
        QVERIFY(canvasSizeProblem(8192, 8192).isEmpty());
        for (const QSize bad : {QSize(0, 5), QSize(5, 0), QSize(-1, 5), QSize(16385, 1), QSize(1, 16385), QSize(9000, 9000)})
            QVERIFY2(canvasSizeProblem(bad.width(), bad.height()) == "A canvas is at most 16384 px a side and 67 megapixels.", qPrintable(QString("%1x%2").arg(bad.width()).arg(bad.height())));
        QVERIFY(!blankCanvas(20000, 10, {}));
    }

    void aTransparentCanvasIsOneEmptyLayer() {
        const Result<PaintDocument> made = blankCanvas(40, 30, {});
        QVERIFY(made);
        QCOMPARE(made->width(), 40);
        QCOMPARE(made->height(), 30);
        QCOMPARE(made->layerCount(), 1);
        QCOMPARE(made->layer(0).name, QString("Layer 1"));
        QCOMPARE(pixel(made.value(), 5, 5).a, uint8_t(0));
    }

    void aFilledCanvasIsOneOpaqueBackgroundLayerWithNoHistory() {
        const Result<PaintDocument> made = blankCanvas(70, 66, CanvasBackground{true, Rgba{10, 20, 30, 255}});
        QVERIFY(made);
        QCOMPARE(made->layerCount(), 1);
        QCOMPARE(made->layer(0).name, QString("Background"));
        QCOMPARE(pixel(made.value(), 0, 0), (Rgba{10, 20, 30, 255}));
        QCOMPARE(pixel(made.value(), 69, 65), (Rgba{10, 20, 30, 255})); // the last tile's edge
        QVERIFY(!made->canUndo());
    }

    void pixelsBecomeOnePaintLayerUnderTheSameCaps() {
        std::vector<uint8_t> rgba(4 * 3 * 2, 0);
        rgba[0] = 255; rgba[3] = 255;       // (0,0) opaque red
        rgba[4 + 1] = 200; rgba[4 + 3] = 128; // (1,0) half-transparent green
        const Result<PaintDocument> made = canvasFromPixels(3, 2, rgba, "Pasted");
        QVERIFY(made);
        QCOMPARE(made->layer(0).name, QString("Pasted"));
        QCOMPARE(pixel(made.value(), 0, 0), (Rgba{255, 0, 0, 255}));
        QCOMPARE(pixel(made.value(), 1, 0), (Rgba{0, 200, 0, 128}));
        QCOMPARE(pixel(made.value(), 2, 1).a, uint8_t(0));
        QVERIFY(!canvasFromPixels(16385, 1, {}, "x"));
    }
};

QTEST_GUILESS_MAIN(TestNewCanvas)
#include "test_new_canvas.moc"
