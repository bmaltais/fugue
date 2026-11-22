// The paint canvas's core (#181, spec 14.4): layers of paint, picture and mask over one size, one
// shared brush and one history, flattened to a picture. Plain numbers, no window.
//
// Seam: engine/paint_document.h (and the four-channel buffer of engine/mask_paint.h).

#include <QtTest>

#include "engine/mask_paint.h"
#include "engine/paint_document.h"
#include "engine/vips.h"

using namespace fugue::mask;

namespace {

void stroke(PaintDocument &d, const std::vector<Point> &points) {
    QVERIFY(d.beginStroke());
    for (const Point &p : points)
        d.strokeTo(p);
    d.endStroke();
}

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

} // namespace

class TestPaintDocument : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(fugue::initVips()); }

    // ---- the four-channel buffer ---------------------------------------------------------------

    void aColorStrokePaintsTheBrushColor() {
        MaskPaint m(32, 32, 4);
        Brush b{10, 1, 1, {200, 50, 10, 255}};
        m.beginStroke(Mode::Paint, b);
        m.strokeTo({16, 16});
        m.endStroke();
        QCOMPARE(m.rgbaAt(16, 16), (Rgba{200, 50, 10, 255}));
        QCOMPARE(m.rgbaAt(0, 0).a, uint8_t(0));
        QVERIFY(!m.isEmpty());
    }

    void aStrokeDoesNotBuildUpOverItself() {
        MaskPaint m(64, 16, 4);
        Brush b{12, 1, 0.5, {255, 0, 0, 255}};
        m.beginStroke(Mode::Paint, b);
        for (int x = 8; x <= 56; x += 2)
            m.strokeTo({double(x), 8});
        m.endStroke();
        QVERIFY(std::abs(int(m.rgbaAt(30, 8).a) - 128) <= 1);
    }

    void aSecondStrokeDoesBuildUpAndKeepsTheColorStraight() {
        MaskPaint m(16, 16, 4);
        Brush b{20, 1, 0.5, {0, 0, 255, 255}};
        for (int i = 0; i < 2; ++i) {
            m.beginStroke(Mode::Paint, b);
            m.strokeTo({8, 8});
            m.endStroke();
        }
        const Rgba p = m.rgbaAt(8, 8);
        QVERIFY(std::abs(int(p.a) - 191) <= 1); // 1 - 0.5 x 0.5
        QCOMPARE(p.b, uint8_t(255));
        QCOMPARE(p.r, uint8_t(0));
    }

    void erasingTakesAlphaAndKeepsTheColor() {
        MaskPaint m(16, 16, 4);
        m.fill({10, 20, 30, 255});
        m.beginStroke(Mode::Erase, Brush{40, 1, 1, {}});
        m.strokeTo({8, 8});
        m.endStroke();
        QCOMPARE(m.rgbaAt(8, 8).a, uint8_t(0));
        QCOMPARE(m.rgbaAt(8, 8).g, uint8_t(20));
    }

    void colorBufferUndoAndRedoAreExact() {
        MaskPaint m(200, 100, 4);
        const std::vector<uint8_t> blank = m.pixels();
        m.beginStroke(Mode::Paint, Brush{30, 0.5, 0.8, {9, 99, 199, 255}});
        for (int x = 20; x < 180; x += 7)
            m.strokeTo({double(x), 50});
        m.endStroke();
        const std::vector<uint8_t> painted = m.pixels();
        QVERIFY(painted != blank);
        m.undo();
        QCOMPARE(m.pixels(), blank);
        m.redo();
        QCOMPARE(m.pixels(), painted);
    }

    void aShapeFillsWithTheColor() {
        MaskPaint m(20, 20, 4);
        m.fillRect(5, 5, 10, 10, Mode::Paint, 1, {1, 2, 3, 255});
        QCOMPARE(m.rgbaAt(10, 10), (Rgba{1, 2, 3, 255}));
        QCOMPARE(m.rgbaAt(2, 2).a, uint8_t(0));
        m.invert(); // colors flip, alpha stays
        QCOMPARE(m.rgbaAt(10, 10), (Rgba{254, 253, 252, 255}));
        QCOMPARE(m.rgbaAt(2, 2).a, uint8_t(0));
    }

    void aOneChannelBufferIsAsItWas() {
        MaskPaint m(16, 16);
        QCOMPARE(m.channels(), 1);
        m.beginStroke(Mode::Paint, Brush{10, 1, 1, {1, 2, 3, 4}}); // the color is ignored
        m.strokeTo({8, 8});
        m.endStroke();
        QCOMPARE(m.at(8, 8), uint8_t(255));
    }

    void colorBuffersRoundTripAsPngAndScale() {
        MaskPaint m(8, 4, 4);
        m.fill({10, 20, 30, 200});
        const auto png = m.toPng();
        QVERIFY(png);
        const auto back = MaskPaint::fromPicture(*png);
        QVERIFY(back);
        QCOMPARE(back->channels(), 4);
        QCOMPARE(back->rgbaAt(3, 2), (Rgba{10, 20, 30, 200}));
        const MaskPaint big = m.scaled(16, 8);
        QCOMPARE(big.rgbaAt(15, 7), (Rgba{10, 20, 30, 200}));
        // a gray mask PNG reads as opaque gray
        MaskPaint gray(4, 4);
        gray.fill();
        const auto g = MaskPaint::fromPicture(*gray.toPng());
        QVERIFY(g);
        QCOMPARE(g->rgbaAt(1, 1), (Rgba{255, 255, 255, 255}));
    }

    // ---- layers --------------------------------------------------------------------------------

    void layersStackAndTheNewestIsActive() {
        PaintDocument d(32, 32);
        const int a = d.addLayer(LayerKind::Paint, "Sketch");
        const int b = d.addLayer(LayerKind::Mask);
        QCOMPARE(d.layerCount(), 2);
        QCOMPARE(d.layer(0).name, QString("Sketch"));
        QCOMPARE(d.layer(1).kind, LayerKind::Mask);
        QCOMPARE(d.activeLayer(), b);
        QVERIFY(d.setActiveLayer(a));
        QVERIFY(!d.setActiveLayer(99));
        QCOMPARE(d.layer(5).id, 0);
    }

    void theBrushAndColorAreSharedByEveryLayer() {
        PaintDocument d(32, 32);
        const int a = d.addLayer(LayerKind::Paint);
        const int b = d.addLayer(LayerKind::Paint);
        d.brush = Brush{8, 1, 1, {255, 0, 0, 255}};
        stroke(d, {{8, 8}});
        d.setActiveLayer(a);
        d.brush.color = {0, 255, 0, 255};
        stroke(d, {{24, 24}});
        QCOMPARE(d.pixels(b)->rgbaAt(8, 8), (Rgba{255, 0, 0, 255}));
        QCOMPARE(d.pixels(a)->rgbaAt(24, 24), (Rgba{0, 255, 0, 255}));
        QCOMPARE(d.pixels(a)->rgbaAt(8, 8).a, uint8_t(0));
    }

    void aPictureLayerIsScaledAndNotPaintedOn() {
        PaintDocument d(8, 8);
        const int p = d.addLayer(LayerKind::Picture, "Photo", solid(2, 2, {5, 6, 7, 255}));
        QCOMPARE(d.pixels(p)->width(), 8);
        QCOMPARE(d.pixels(p)->rgbaAt(7, 7), (Rgba{5, 6, 7, 255}));
        QVERIFY(!d.canPaint());
        QVERIFY(!d.beginStroke());
        QVERIFY(!d.fillRect(0, 0, 4, 4));
        QVERIFY(!d.clearLayer());
        QCOMPARE(d.undoSteps(), size_t(1)); // the add only
    }

    void paintOverAPictureFlattensOver() {
        PaintDocument d(16, 16);
        d.addLayer(LayerKind::Picture, "Photo", solid(16, 16, {0, 0, 255, 255}));
        d.addLayer(LayerKind::Paint);
        d.brush = Brush{6, 1, 1, {255, 0, 0, 255}};
        stroke(d, {{8, 8}});
        const MaskPaint flat = d.flatten();
        QCOMPARE(flat.channels(), 4);
        QCOMPARE(flat.rgbaAt(8, 8), (Rgba{255, 0, 0, 255}));
        QCOMPARE(flat.rgbaAt(0, 0), (Rgba{0, 0, 255, 255}));
    }

    void layerOpacityVisibilityAndOrderChangeTheFlatPicture() {
        PaintDocument d(4, 4);
        const int under = d.addLayer(LayerKind::Picture, "Under", solid(4, 4, {0, 0, 0, 255}));
        const int over = d.addLayer(LayerKind::Picture, "Over", solid(4, 4, {255, 255, 255, 255}));
        QCOMPARE(d.flatten().rgbaAt(1, 1), (Rgba{255, 255, 255, 255}));
        d.setOpacity(over, 0.5);
        const Rgba half = d.flatten().rgbaAt(1, 1);
        QVERIFY(std::abs(int(half.r) - 128) <= 1);
        QCOMPARE(half.a, uint8_t(255));
        d.setVisible(over, false);
        QCOMPARE(d.flatten().rgbaAt(1, 1), (Rgba{0, 0, 0, 255}));
        d.setVisible(over, true);
        d.setOpacity(over, 1);
        QVERIFY(d.moveLayer(over, 0));
        QCOMPARE(d.positionOf(under), 1);
        QCOMPARE(d.flatten().rgbaAt(1, 1), (Rgba{0, 0, 0, 255}));
    }

    void aTranslucentLayerOverNothingKeepsItsColorStraight() {
        PaintDocument d(2, 2);
        d.addLayer(LayerKind::Picture, "P", solid(2, 2, {200, 100, 50, 128}));
        QCOMPARE(d.flatten().rgbaAt(0, 0), (Rgba{200, 100, 50, 128}));
    }

    void maskLayersAreTheirOwnOutput() {
        PaintDocument d(16, 16);
        d.addLayer(LayerKind::Picture, "P", solid(16, 16, {1, 2, 3, 255}));
        const int m = d.addLayer(LayerKind::Mask);
        d.brush = Brush{8, 1, 1, {255, 0, 0, 255}};
        stroke(d, {{8, 8}});
        QCOMPARE(d.pixels(m)->channels(), 1);
        const MaskPaint mask = d.flattenMask();
        QCOMPARE(mask.channels(), 1);
        QCOMPARE(mask.at(8, 8), uint8_t(255));
        QCOMPARE(mask.at(0, 0), uint8_t(0));
        QCOMPARE(d.flatten().rgbaAt(8, 8), (Rgba{1, 2, 3, 255})); // the mask is not in the picture
        d.setOpacity(m, 0.5);
        QVERIFY(std::abs(int(d.flattenMask().at(8, 8)) - 128) <= 1);
        d.setVisible(m, false);
        QVERIFY(d.flattenMask().isEmpty());
    }

    void flattenGivesAPng() {
        PaintDocument d(4, 4);
        d.addLayer(LayerKind::Picture, "P", solid(4, 4, {9, 8, 7, 255}));
        const auto png = d.flattenToPng();
        QVERIFY(png);
        const auto back = MaskPaint::fromPicture(*png);
        QVERIFY(back);
        QCOMPARE(back->rgbaAt(2, 2), (Rgba{9, 8, 7, 255}));
    }

    // ---- one history ---------------------------------------------------------------------------

    void undoGoesBackThroughLayersInOrder() {
        PaintDocument d(32, 32);
        const int a = d.addLayer(LayerKind::Paint);
        d.brush = Brush{8, 1, 1, {255, 0, 0, 255}};
        stroke(d, {{8, 8}});
        const int b = d.addLayer(LayerKind::Paint);
        stroke(d, {{24, 24}});
        QCOMPARE(d.undoSteps(), size_t(4));
        d.undo(); // the stroke on b
        QCOMPARE(d.pixels(b)->rgbaAt(24, 24).a, uint8_t(0));
        QCOMPARE(d.pixels(a)->rgbaAt(8, 8).a, uint8_t(255));
        d.undo(); // adding b
        QCOMPARE(d.layerCount(), 1);
        QCOMPARE(d.activeLayer(), a);
        d.undo(); // the stroke on a
        QCOMPARE(d.pixels(a)->rgbaAt(8, 8).a, uint8_t(0));
        d.redo();
        d.redo();
        QCOMPARE(d.layerCount(), 2);
        QCOMPARE(d.activeLayer(), b);
        d.redo();
        QCOMPARE(d.pixels(b)->rgbaAt(24, 24).a, uint8_t(255));
        QVERIFY(!d.canRedo());
    }

    void removingALayerIsUndoneWithItsPaintAndItsHistory() {
        PaintDocument d(32, 32);
        const int a = d.addLayer(LayerKind::Paint);
        const int b = d.addLayer(LayerKind::Paint);
        d.brush = Brush{8, 1, 1, {0, 0, 255, 255}};
        stroke(d, {{16, 16}});
        QVERIFY(d.removeLayer(b));
        QCOMPARE(d.layerCount(), 1);
        QCOMPARE(d.activeLayer(), a);
        QCOMPARE(d.flatten().rgbaAt(16, 16).a, uint8_t(0));
        d.undo();
        QCOMPARE(d.layerCount(), 2);
        QCOMPARE(d.activeLayer(), b);
        QCOMPARE(d.flatten().rgbaAt(16, 16), (Rgba{0, 0, 255, 255}));
        d.undo(); // and the stroke inside it is still a step of its own
        QCOMPARE(d.flatten().rgbaAt(16, 16).a, uint8_t(0));
        d.redo();
        d.redo();
        QCOMPARE(d.layerCount(), 1);
        d.undo();
        d.undo();
        d.undo(); // adding b
        QCOMPARE(d.layerCount(), 1);
    }

    void movingALayerIsUndone() {
        PaintDocument d(4, 4);
        const int a = d.addLayer(LayerKind::Paint);
        const int b = d.addLayer(LayerKind::Paint);
        d.moveLayer(a, 1);
        QCOMPARE(d.positionOf(a), 1);
        d.undo();
        QCOMPARE(d.positionOf(a), 0);
        QCOMPARE(d.positionOf(b), 1);
        d.redo();
        QCOMPARE(d.positionOf(a), 1);
    }

    void aNewStepEndsTheRedo() {
        PaintDocument d(16, 16);
        d.addLayer(LayerKind::Paint);
        d.brush = Brush{4, 1, 1, {1, 1, 1, 255}};
        stroke(d, {{4, 4}});
        d.undo();
        QVERIFY(d.canRedo());
        stroke(d, {{10, 10}});
        QVERIFY(!d.canRedo());
    }

    void aStrokeThatChangesNothingIsNoStep() {
        PaintDocument d(16, 16);
        d.addLayer(LayerKind::Paint);
        stroke(d, {{-100, -100}}); // outside
        d.beginStroke();
        d.endStroke();
        QCOMPARE(d.undoSteps(), size_t(1));
    }

    void wholeLayerOperationsAreSteps() {
        PaintDocument d(8, 8);
        d.addLayer(LayerKind::Paint);
        d.brush.color = {10, 20, 30, 255};
        QVERIFY(d.fillLayer());
        QCOMPARE(d.flatten().rgbaAt(3, 3), (Rgba{10, 20, 30, 255}));
        QVERIFY(d.invertLayer());
        QCOMPARE(d.flatten().rgbaAt(3, 3), (Rgba{245, 235, 225, 255}));
        QVERIFY(d.clearLayer());
        QVERIFY(d.flatten().isEmpty());
        d.undo();
        d.undo();
        QCOMPARE(d.flatten().rgbaAt(3, 3), (Rgba{10, 20, 30, 255}));
        QVERIFY(d.fillRect(0, 0, 4, 4));
        QVERIFY(d.fillPolygon({{0, 0}, {8, 0}, {0, 8}}));
    }

    void noUndoWhileAStrokeIsOpen() {
        PaintDocument d(16, 16);
        d.addLayer(LayerKind::Paint);
        stroke(d, {{4, 4}});
        QVERIFY(d.canUndo());
        d.beginStroke();
        QVERIFY(!d.canUndo());
        QVERIFY(!d.addLayer(LayerKind::Paint));
        d.endStroke();
        QVERIFY(d.canUndo());
    }

    // ---- a rectangle of the result (what a view draws) -----------------------------------------

    void flattenAreaIsTheSameAsTheWholeCutOut() {
        PaintDocument d(40, 30);
        d.addLayer(LayerKind::Picture, "p", solid(40, 30, {10, 200, 30, 255}));
        d.addLayer(LayerKind::Paint);
        d.brush = {12, 1, 0.7, {200, 20, 20, 255}};
        stroke(d, {{5, 5}, {30, 22}});
        const int mask = d.addLayer(LayerKind::Mask);
        QVERIFY(d.setOpacity(mask, 0.5));
        d.brush = {10, 1, 1, {}};
        stroke(d, {{20, 10}, {36, 10}});
        const MaskPaint whole = d.flatten(), wholeMask = d.flattenMask();
        const IRect area{7, 3, 25, 20};
        const MaskPaint part = d.flattenArea(area), partMask = d.flattenMaskArea(area);
        QCOMPARE(part.width(), 25);
        QCOMPARE(part.height(), 20);
        QCOMPARE(partMask.width(), 25);
        for (int y = 0; y < area.height; ++y)
            for (int x = 0; x < area.width; ++x) {
                QCOMPARE(part.rgbaAt(x, y), whole.rgbaAt(area.x + x, area.y + y));
                QCOMPARE(partMask.at(x, y), wholeMask.at(area.x + x, area.y + y));
            }
        // A rectangle past the edge is cut to the canvas; one outside it is a single empty pixel.
        QCOMPARE(d.flattenArea({30, 20, 50, 50}).width(), 10);
        QCOMPARE(d.flattenArea({30, 20, 50, 50}).height(), 10);
        QCOMPARE(d.flattenArea({100, 100, 5, 5}).rgbaAt(0, 0).a, uint8_t(0));
    }

};


QTEST_GUILESS_MAIN(TestPaintDocument)
#include "test_paint_document.moc"
