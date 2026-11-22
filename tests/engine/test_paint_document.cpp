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


    // ---- blend modes and layer masks (#226, spec 14.4) ------------------------------------------

    // A gray backdrop of 0.4 (102) under a layer of 0.6 (153), both opaque: the W3C formulas give
    // these bytes (spec 14.4 lists the same table).
    void eachBlendModeGivesTheValueOfItsFormula() {
        const struct { Blend blend; uint8_t expected; } rows[] = {
            {Blend::Normal, 153},   {Blend::Multiply, 61}, {Blend::Screen, 194}, {Blend::Overlay, 122},
            {Blend::SoftLight, 114}, {Blend::Darken, 102}, {Blend::Lighten, 153}, {Blend::Add, 255},
        };
        for (const auto &row : rows) {
            PaintDocument d(2, 2);
            d.loadLayer(LayerKind::Paint, "under", true, 1, solid(2, 2, {102, 102, 102, 255}));
            const int top = d.loadLayer(LayerKind::Paint, "over", true, 1, solid(2, 2, {153, 153, 153, 255}));
            QVERIFY(d.setBlend(top, row.blend));
            QCOMPARE(d.layer(1).blend(), row.blend);
            const Rgba got = d.flatten().rgbaAt(1, 1);
            QVERIFY2(got.r == row.expected && got.g == row.expected && got.b == row.expected && got.a == 255,
                     qPrintable(QStringLiteral("%1: %2, not %3").arg(blendId(row.blend)).arg(got.r).arg(row.expected)));
        }
    }

    void theFormulasAtTheirBreakPoints() {
        // Overlay switches on the backdrop at 0.5, soft light on the layer at 0.5 and its D at 0.25.
        QCOMPARE(blendChannel(Blend::Overlay, 0.75, 0.5), 0.75);
        QCOMPARE(blendChannel(Blend::Overlay, 0.25, 0.5), 0.25);
        QCOMPARE(blendChannel(Blend::SoftLight, 0.2, 0.25), 0.2 - 0.5 * 0.2 * 0.8);
        QCOMPARE(blendChannel(Blend::SoftLight, 0.2, 1.0), 0.2 + (((16 * 0.2 - 12) * 0.2 + 4) * 0.2 - 0.2));
        QCOMPARE(blendChannel(Blend::Add, 0.7, 0.6), 1.0);
    }

    // Half the layer's opacity mixes the blended color half and half with the backdrop; over nothing,
    // the blend has nothing to work on and the layer shows as it is.
    void aBlendRespectsOpacityAndATransparentBackdrop() {
        PaintDocument d(2, 2);
        d.loadLayer(LayerKind::Paint, "under", true, 1, solid(2, 2, {102, 102, 102, 255}));
        const int top = d.loadLayer(LayerKind::Paint, "over", true, 0.5, solid(2, 2, {153, 153, 153, 255}));
        d.setBlend(top, Blend::Multiply);
        QCOMPARE(d.flatten().rgbaAt(0, 0), (Rgba{82, 82, 82, 255})); // 0.5 * 0.24 + 0.5 * 0.4 = 0.32

        PaintDocument alone(2, 2);
        const int only = alone.loadLayer(LayerKind::Paint, "over", true, 1, solid(2, 2, {153, 10, 200, 128}));
        alone.setBlend(only, Blend::Multiply);
        QCOMPARE(alone.flatten().rgbaAt(0, 0), (Rgba{153, 10, 200, 128}));
    }

    void normalIsTheSameAsBefore() {
        PaintDocument d(2, 2);
        d.loadLayer(LayerKind::Paint, "under", true, 1, solid(2, 2, {10, 200, 30, 200}));
        d.loadLayer(LayerKind::Paint, "over", true, 0.7, solid(2, 2, {250, 20, 90, 100}));
        const Rgba before = d.flatten().rgbaAt(0, 0);
        d.setBlend(d.layer(1).id, Blend::Multiply);
        d.setBlend(d.layer(1).id, Blend::Normal);
        QCOMPARE(d.layer(1).compositeOp, QString());
        QCOMPARE(d.flatten().rgbaAt(0, 0), before);
    }

    // An op Fugue does not draw is kept as it is and composites as Normal.
    void anUnknownCompositeOpIsKeptAndDrawnAsNormal() {
        bool known = true;
        QCOMPARE(blendOfOp(QStringLiteral("svg:color-dodge"), &known), Blend::Normal);
        QVERIFY(!known);
        QCOMPARE(blendOfOp(QStringLiteral("svg:plus")), Blend::Add);
        QCOMPARE(opOfBlend(Blend::SoftLight), QStringLiteral("svg:soft-light"));
        PaintDocument d(2, 2);
        d.loadLayer(LayerKind::Paint, "under", true, 1, solid(2, 2, {102, 102, 102, 255}));
        const int top = d.loadLayer(LayerKind::Paint, "over", true, 1, solid(2, 2, {153, 153, 153, 255}));
        d.setCompositeOp(top, QStringLiteral("svg:color-dodge"));
        QCOMPARE(d.layer(1).compositeOp, QStringLiteral("svg:color-dodge"));
        QCOMPARE(d.flatten().rgbaAt(0, 0).r, uint8_t(153));
        d.setCompositeOp(top, QStringLiteral("svg:src-over"));
        QCOMPARE(d.layer(1).compositeOp, QString());
    }

    // White shows the layer, black hides it, grey is in between: the layer's alpha times the mask.
    void aLayerMaskMultipliesTheLayersAlpha() {
        PaintDocument d(3, 1);
        d.loadLayer(LayerKind::Paint, "under", true, 1, solid(3, 1, {0, 0, 0, 255}));
        const int top = d.loadLayer(LayerKind::Paint, "over", true, 1, solid(3, 1, {255, 255, 255, 255}));
        QVERIFY(d.setLayerMask(top, MaskPaint(3, 1, std::vector<uint8_t>{255, 128, 0}, 1)));
        QVERIFY(d.layer(1).hasMask);
        const MaskPaint flat = d.flatten();
        QCOMPARE(flat.rgbaAt(0, 0), (Rgba{255, 255, 255, 255}));
        QCOMPARE(flat.rgbaAt(1, 0), (Rgba{128, 128, 128, 255}));
        QCOMPARE(flat.rgbaAt(2, 0), (Rgba{0, 0, 0, 255}));
        // Off, the layer shows whole and the mask is kept; removed, it is gone.
        QVERIFY(d.setMaskEnabled(top, false));
        QCOMPARE(d.flatten().rgbaAt(2, 0), (Rgba{255, 255, 255, 255}));
        QVERIFY(d.maskPixels(top) != nullptr);
        QVERIFY(d.removeLayerMask(top));
        QVERIFY(!d.layer(1).hasMask);
        QVERIFY(d.maskPixels(top) == nullptr);
        // A mask layer cannot own one, and the mask output does not see a layer mask.
        const int maskLayer = d.addLayer(LayerKind::Mask);
        QVERIFY(!d.setLayerMask(maskLayer, MaskPaint(3, 1, 1)));
    }

    void aLayerMaskWithABlendModeAndOpacity() {
        PaintDocument d(1, 1);
        d.loadLayer(LayerKind::Paint, "under", true, 1, solid(1, 1, {102, 102, 102, 255}));
        const int top = d.loadLayer(LayerKind::Paint, "over", true, 1, solid(1, 1, {153, 153, 153, 255}));
        d.setBlend(top, Blend::Screen);
        d.setLayerMask(top, MaskPaint(1, 1, std::vector<uint8_t>{255}, 1));
        d.setOpacity(top, 0.5);
        // 0.5 * 0.76 + 0.5 * 0.4 = 0.58
        QCOMPARE(d.flatten().rgbaAt(0, 0).r, uint8_t(148));
    }

    void aLayersSettingsGoWithItWhenScaledRemovedAndUndone() {
        PaintDocument d(4, 4);
        LayerInfo info;
        info.name = "Fill 1";
        info.compositeOp = "svg:multiply";
        info.producer = "canvas-write:write";
        info.created = "2026-11-22T10:00:00.000Z";
        info.maskEnabled = false;
        info.hasMask = true;
        const MaskPaint mask(4, 4, std::vector<uint8_t>(16, 200), 1);
        const int id = d.loadLayer(info, solid(4, 4, {9, 9, 9, 255}), &mask);
        const LayerInfo loaded = d.layer(0);
        QCOMPARE(loaded.compositeOp, QStringLiteral("svg:multiply"));
        QCOMPARE(loaded.producer, info.producer);
        QCOMPARE(loaded.created, info.created);
        QVERIFY(loaded.hasMask);
        QVERIFY(!loaded.maskEnabled);
        const PaintDocument bigger = d.scaledTo(8, 8);
        QCOMPARE(bigger.layer(0).compositeOp, QStringLiteral("svg:multiply"));
        QVERIFY(bigger.maskPixels(bigger.layer(0).id) != nullptr);
        QCOMPARE(bigger.maskPixels(bigger.layer(0).id)->width(), 8);
        QVERIFY(d.removeLayer(id));
        d.undo();
        QCOMPARE(d.layerCount(), 1);
        QVERIFY(d.maskPixels(id) != nullptr);
        QCOMPARE(d.maskPixels(id)->at(1, 1), uint8_t(200));
    }
};


QTEST_GUILESS_MAIN(TestPaintDocument)
#include "test_paint_document.moc"
