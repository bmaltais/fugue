// Moving, scaling, turning and flipping a layer on a canvas (#236, epic #206, spec 14.5): the resampling's numbers
// (engine/canvas_transform.h), and CanvasStore::transformLayer, which draws a layer again from its kept original so a later
// transform starts from the original pixels, takes the layer mask along, and is ONE revision.
//
// Seam: the pure functions on pixels, and the store with a folder and a clock of the test's own.

#include <QTemporaryDir>
#include <QtTest>

#include "engine/canvas_layer_ops.h"
#include "engine/canvas_store.h"
#include "engine/canvas_transform.h"

using namespace fugue;
using namespace fugue::canvas;
using namespace fugue::mask;

namespace {

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

MaskPaint ramp(int w, int h) { // pixel (x, y) is (x * 10, y * 10, 7, 255)
    std::vector<uint8_t> px;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (uint8_t v : {uint8_t(x * 10), uint8_t(y * 10), uint8_t(7), uint8_t(255)})
                px.push_back(v);
    return MaskPaint(w, h, px, 4);
}

Rgba at(const MaskPaint &m, int x, int y) {
    const size_t i = (size_t(y) * m.width() + x) * 4;
    return {m.pixels()[i], m.pixels()[i + 1], m.pixels()[i + 2], m.pixels()[i + 3]};
}

LayerTransform place(int w, int h, double cx, double cy, double width, double height, double angle = 0, ResampleFilter filter = ResampleFilter::Lanczos) {
    LayerTransform t;
    t.cx = cx;
    t.cy = cy;
    t.width = width;
    t.height = height;
    t.angle = angle;
    t.filter = filter;
    Q_UNUSED(w) Q_UNUSED(h)
    return t;
}

bool same(const MaskPaint &a, const MaskPaint &b) { return a.pixels() == b.pixels(); }

} // namespace

class TestCanvasTransform : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    int m_tick = 0;
    Clock clock() { return [this] { return QDateTime(QDate(2026, 11, 23), QTime(9, 0, 0), QTimeZone::utc()).addSecs(m_tick++); }; }
    CanvasStore store() { return CanvasStore(m_dir.path(), clock()); }
    CanvasInfo made(CanvasStore &s, int w = 100, int h = 80) {
        PaintDocument d(w, h);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(w, h, {10, 20, 30, 255}));
        return s.create("c", d, "test").value();
    }
    // The shown pixels of one layer (bottom first) of a revision.
    MaskPaint shown(CanvasStore &s, const QString &id, const QString &rev, int position) {
        const PaintDocument doc = s.load(id, rev).value();
        return *doc.pixels(doc.layer(position).id);
    }
    TileReader reader(CanvasStore &s) {
        return [&s](const QString &hash, int w, int h, int c) { return s.tilePixels(hash, w, h, c); };
    }

private slots:
    // ---- the pixels ---------------------------------------------------------------------------------------------

    void nearestRepeatsPixelsAndKeepsEachColor() {
        const MaskPaint two = MaskPaint(2, 2, std::vector<uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 9, 9, 9, 255}, 4);
        const MaskPaint big = renderPlaced(two, place(8, 8, 4, 4, 4, 4, 0, ResampleFilter::Nearest), 8, 8).value();
        QCOMPARE(at(big, 2, 2), (Rgba{255, 0, 0, 255})); // the 4 x 4 picture spans 2..5
        QCOMPARE(at(big, 3, 2), (Rgba{255, 0, 0, 255}));
        QCOMPARE(at(big, 4, 2), (Rgba{0, 255, 0, 255}));
        QCOMPARE(at(big, 2, 4), (Rgba{0, 0, 255, 255}));
        QCOMPARE(at(big, 5, 5), (Rgba{9, 9, 9, 255}));
        QCOMPARE(at(big, 1, 2).a, uint8_t(0));
        QCOMPARE(at(big, 6, 5).a, uint8_t(0));
    }

    void aFlatColorStaysFlatThroughEveryFilterAndScale() {
        const MaskPaint flat = solid(16, 16, {200, 100, 50, 255});
        for (const ResampleFilter filter : {ResampleFilter::Lanczos, ResampleFilter::Bicubic, ResampleFilter::Nearest})
            for (const double side : {5.0, 16.0, 37.0}) {
                const MaskPaint out = renderPlaced(flat, place(40, 40, 20, 20, side, side, 0, filter), 40, 40).value();
                const int left = int(std::lround(20 - side / 2)), at0 = left + int(side) / 2;
                QCOMPARE(at(out, at0, at0), (Rgba{200, 100, 50, 255}));
            }
    }

    void shrinkingAveragesInsteadOfSkipping() {
        // A 1-pixel checker of 0 and 254 shrunk by 2 is mid gray under Lanczos, not black or white as picking would give.
        std::vector<uint8_t> px;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                for (uint8_t v : {uint8_t((x + y) % 2 ? 254 : 0), uint8_t((x + y) % 2 ? 254 : 0), uint8_t((x + y) % 2 ? 254 : 0), uint8_t(255)})
                    px.push_back(v);
        const MaskPaint out = renderPlaced(MaskPaint(16, 16, px, 4), place(8, 8, 4, 4, 8, 8), 8, 8).value();
        for (int i = 2; i < 6; ++i)
            QVERIFY2(std::abs(int(at(out, i, 3).r) - 127) < 12, qPrintable(QString::number(at(out, i, 3).r)));
    }

    void transparentPixelsLeakNoColorIntoTheirNeighbours() {
        // Left pixel opaque red, right pixel invisible blue: upscaled, every visible pixel is still red.
        const MaskPaint src(2, 1, std::vector<uint8_t>{255, 0, 0, 255, 0, 0, 255, 0}, 4);
        const MaskPaint out = renderPlaced(src, place(8, 2, 4, 1, 8, 2), 8, 2).value();
        for (int x = 0; x < 8; ++x)
            if (at(out, x, 0).a > 0) {
                QCOMPARE(at(out, x, 0).r, uint8_t(255));
                QVERIFY2(at(out, x, 0).b <= 1, "blue leaked");
            }
        QVERIFY(at(out, 0, 0).a > 200);
        QVERIFY(at(out, 7, 0).a < 60);
    }

    void aFlipIsExactAndMirrorsTheOtherWay() {
        const MaskPaint src = ramp(4, 3);
        LayerTransform t = place(10, 10, 5, 5, 4, 3);
        t = snappedToPixels(t);
        t.flipX = true;
        const MaskPaint out = renderPlaced(src, t, 10, 10).value();
        const int left = int(std::lround(t.cx - 2)), top = int(std::lround(t.cy - 1.5));
        QCOMPARE(at(out, left, top).r, uint8_t(30)); // the picture's last column first
        QCOMPARE(at(out, left + 3, top + 2), (Rgba{0, 20, 7, 255}));
        LayerTransform v = t;
        v.flipX = false;
        v.flipY = true;
        QCOMPARE(at(renderPlaced(src, v, 10, 10).value(), left, top), (Rgba{0, 20, 7, 255}));
        // On screen, flipping twice is no flip, and it reverses the turn.
        LayerTransform turned = t;
        turned.angle = 30;
        QVERIFY(flippedOnScreen(flippedOnScreen(turned, true), true) == turned.normalized());
        QCOMPARE(flippedOnScreen(turned, true).angle, -30.0);
    }

    void aQuarterTurnIsExact() {
        const MaskPaint src = ramp(4, 2);
        LayerTransform t = place(10, 10, 5, 5, 4, 2);
        t.angle = 90;
        t = snappedToPixels(t);
        const MaskPaint out = renderPlaced(src, t, 10, 10).value();
        // Turned clockwise, the picture is 2 wide and 4 high; its top left pixel is now at the top right, and the left column at the top.
        const int left = int(std::lround(t.cx - 1)), top = int(std::lround(t.cy - 2));
        QCOMPARE(at(out, left + 1, top), (Rgba{0, 0, 7, 255}));      // source (0, 0)
        QCOMPARE(at(out, left, top), (Rgba{0, 10, 7, 255}));         // source (0, 1)
        QCOMPARE(at(out, left + 1, top + 3), (Rgba{30, 0, 7, 255})); // source (3, 0)
        QCOMPARE(at(out, left, top + 3), (Rgba{30, 10, 7, 255}));    // source (3, 1)
        QCOMPARE(at(out, left - 1, top).a, uint8_t(0));
        // Four quarter turns come back to the start.
        LayerTransform again = place(10, 10, 5, 5, 4, 2);
        again = snappedToPixels(again);
        LayerTransform start = again;
        for (int i = 0; i < 4; ++i)
            again = turnedQuarter(again, true);
        QVERIFY(again == start);
        QVERIFY(same(renderPlaced(src, again, 10, 10).value(), renderPlaced(src, start, 10, 10).value()));
        // A half turn is both flips.
        LayerTransform half = start;
        half.angle = 180;
        LayerTransform both = start;
        both.flipX = both.flipY = true;
        QVERIFY(same(renderPlaced(src, half, 10, 10).value(), renderPlaced(src, both, 10, 10).value()));
    }

    void aTurnOf45KeepsTheCenterAndLeavesCornersTransparent() {
        const MaskPaint src = solid(20, 20, {255, 0, 0, 255});
        for (const ResampleFilter filter : {ResampleFilter::Lanczos, ResampleFilter::Bicubic, ResampleFilter::Nearest}) {
            const MaskPaint out = renderPlaced(src, place(60, 60, 30, 30, 20, 20, 45, filter), 60, 60).value();
            QCOMPARE(at(out, 30, 30), (Rgba{255, 0, 0, 255}));
            QCOMPARE(at(out, 30, 17), (Rgba{255, 0, 0, 255})); // the diamond reaches 14.1 px up
            QCOMPARE(at(out, 30, 12).a, uint8_t(0));
            QCOMPARE(at(out, 18, 18).a, uint8_t(0)); // the corner of the unturned square
            QCOMPARE(at(out, 0, 0).a, uint8_t(0));
        }
    }

    void onlyWhatTheCanvasShowsIsComputedSoAHugePictureIsCheap() {
        const MaskPaint big = solid(4000, 3000, {1, 2, 3, 255});
        QElapsedTimer timer;
        timer.start();
        const MaskPaint out = renderPlaced(big, place(100, 80, 50, 40, 2000, 1500), 100, 80).value(); // 2:1 shrink, only 100 x 80 drawn
        QCOMPARE(at(out, 50, 40), (Rgba{1, 2, 3, 255}));
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
    }

    // A render is drawn in tiles of the canvas: any tile size gives the same pixels as one whole piece, with every filter, scaled, turned or both.
    void tilesGiveTheSamePixelsAsOneWholeRender() {
        const MaskPaint src = ramp(37, 29);
        for (const ResampleFilter filter : {ResampleFilter::Lanczos, ResampleFilter::Bicubic, ResampleFilter::Nearest})
            for (const double angle : {0.0, 30.0, 90.0, -143.0})
                for (const double scale : {0.4, 1.0, 2.7}) {
                    const LayerTransform t = place(80, 70, 41, 33, 37 * scale, 29 * scale, angle, filter);
                    RenderOptions small;
                    small.tile = 16;
                    RenderOptions whole;
                    whole.tile = 100000;
                    const MaskPaint a = renderPlaced(src, t, 80, 70, small).value(), b = renderPlaced(src, t, 80, 70, whole).value();
                    QVERIFY2(same(a, b), qPrintable(QString("filter %1 angle %2 scale %3").arg(int(filter)).arg(angle).arg(scale)));
                }
        // The mask goes through the same tiles.
        const MaskPaint gray(20, 10, std::vector<uint8_t>(200, 77), 1);
        RenderOptions small;
        small.tile = 16;
        QVERIFY(same(renderPlacedMask(gray, place(50, 40, 25, 20, 40, 20, 20), 50, 40, small).value(), renderPlacedMask(gray, place(50, 40, 25, 20, 40, 20, 20), 50, 40).value()));
    }

    // What a render holds besides the canvas's own bytes is a tile's buffers: a big picture shrunk, or scaled up and turned, stays small.
    void theMemoryOfARenderIsATilesNotThePictures() {
        const MaskPaint big = solid(3000, 2000, {10, 120, 230, 255});
        size_t peak = 0;
        RenderOptions options;
        options.peakBytes = &peak;
        // 2x up and turned 45 degrees: the scaled picture would be 6000 x 4000 x 16 bytes = 384 MB whole.
        const MaskPaint turned = renderPlaced(big, place(600, 500, 300, 250, 6000, 4000, 45), 600, 500, options).value();
        QCOMPARE(at(turned, 300, 250), (Rgba{10, 120, 230, 255}));
        QVERIFY2(peak < size_t(8) << 20, qPrintable(QString::number(peak)));
        // 8x down: the source's rows are taken one at a time, not held.
        peak = 0;
        const MaskPaint small = renderPlaced(big, place(400, 300, 190, 125, 375, 250), 400, 300, options).value();
        QCOMPARE(at(small, 190, 125), (Rgba{10, 120, 230, 255}));
        QVERIFY2(peak < size_t(2) << 20, qPrintable(QString::number(peak)));
    }

    void aPlacementThatCannotBeDrawnIsRefusedInWords() {
        const MaskPaint src = solid(4, 4, {1, 1, 1, 255});
        QVERIFY(!renderPlaced(src, place(10, 10, 5, 5, 0, 4), 10, 10));
        QVERIFY(!renderPlaced(src, place(10, 10, 5, 5, 100000, 4), 10, 10));
        QVERIFY(!renderPlaced(src, place(10, 10, 5, 5, 30000, 30000), 10, 10)); // past the area cap
        QVERIFY(!renderPlaced(src, place(10, 10, std::nan(""), 5, 4, 4), 10, 10));
        QVERIFY(transformProblem(place(10, 10, 5, 5, 4, 4)).isEmpty());
    }

    // ---- the store ----------------------------------------------------------------------------------------------

    void aPastedPictureScaledDownAndBackUpComesBackExactly() {
        CanvasStore s = store();
        const CanvasInfo c = made(s, 100, 80);
        const MaskPaint picture = ramp(40, 40);
        const Commit pasted = s.pasteLayer(c.id, c.head, picture, 50, 40, -1, {}, "canvas-view:paste").value();
        const MaskPaint original = shown(s, c.id, pasted.revision.id, 1);
        const LayerTransform start = s.loadTree(c.id, pasted.revision.id)->layers[1].source->transform;
        // Scale to a quarter of the area: one revision.
        LayerTransform small = start;
        small.width = small.height = 10;
        const Commit shrunk = s.transformLayer(c.id, pasted.revision.id, 1, small, "canvas-view:transform").value();
        QVERIFY(shrunk.move.moved);
        QCOMPARE(shrunk.revision.parent, pasted.revision.id);
        QCOMPARE(shrunk.revision.producer, QStringLiteral("canvas-view:transform"));
        QVERIFY(!same(shown(s, c.id, shrunk.revision.id, 1), original));
        // Back to the original size and place: drawn from the original, so the pixels are the original's again, exactly.
        const Commit back = s.transformLayer(c.id, shrunk.revision.id, 1, start, "canvas-view:transform").value();
        QVERIFY(same(shown(s, c.id, back.revision.id, 1), original));
        // The original never changed.
        const Tree t = s.loadTree(c.id, back.revision.id).value();
        QCOMPARE(t.layers[1].source->tiles, s.loadTree(c.id, pasted.revision.id)->layers[1].source->tiles);
        // The same placement again writes nothing.
        QVERIFY(!s.transformLayer(c.id, back.revision.id, 1, start, "canvas-view:transform")->move.moved);
        QCOMPARE(s.info(c.id)->head, back.revision.id);
        // Undo takes each step back.
        QVERIFY(s.undo(c.id, back.revision.id).value().moved);
        QCOMPARE(s.info(c.id)->head, shrunk.revision.id);
    }

    // A placement read from one revision is never applied to another: the store refuses when the layer is not what the caller saw.
    void aTransformFromAStalePlacementIsRefused() {
        CanvasStore s = store();
        const CanvasInfo c = made(s, 100, 80);
        const Commit pasted = s.pasteLayer(c.id, c.head, solid(30, 20, {9, 9, 9, 255}), 50, 40, -1, {}, "p").value();
        const Tree t = s.loadTree(c.id, pasted.revision.id).value();
        const SeenPlacement seen{t.layers[1].source->transform, true, 30, 20};
        LayerTransform moved = seen.transform;
        moved.cx += 5;
        // Another transform lands first (a second window, a run): the placement the caller saw is old.
        const Commit other = s.transformLayer(c.id, pasted.revision.id, 1, place(0, 0, 60, 40, 30, 20, 90), "t").value();
        QVERIFY(other.move.moved);
        const Result<Commit> late = s.transformLayer(c.id, other.revision.id, 1, moved, "t", &seen);
        QVERIFY(!late);
        QVERIFY(late.error().message.contains("changed since"));
        QCOMPARE(s.info(c.id)->head, other.revision.id);
        // The current one is accepted, and a layer with no original is not what a caller that saw one looked at.
        const Tree now = s.loadTree(c.id, other.revision.id).value();
        const SeenPlacement fresh{now.layers[1].source->transform, true, 30, 20};
        QVERIFY(s.transformLayer(c.id, other.revision.id, 1, moved, "t", &fresh)->move.moved);
        const SeenPlacement none{moved, false, 30, 20};
        QVERIFY(!s.transformLayer(c.id, s.info(c.id)->head, 1, place(0, 0, 10, 10, 30, 20), "t", &none));
    }

    void aBiggerPictureScaledDownShowsAllOfIt() {
        CanvasStore s = store();
        const CanvasInfo c = made(s, 40, 30);
        const Commit pasted = s.pasteLayer(c.id, c.head, solid(400, 300, {250, 0, 0, 255}), 20, 15, -1, {}, "canvas-view:paste").value();
        QCOMPARE(at(shown(s, c.id, pasted.revision.id, 1), 0, 0), (Rgba{250, 0, 0, 255}));
        LayerTransform fit = s.loadTree(c.id, pasted.revision.id)->layers[1].source->transform;
        fit.width = 40;
        fit.height = 30;
        fit.cx = 20;
        fit.cy = 15;
        const Commit scaled = s.transformLayer(c.id, pasted.revision.id, 1, fit, "canvas-view:transform").value();
        const MaskPaint now = shown(s, c.id, scaled.revision.id, 1);
        QCOMPARE(at(now, 0, 0), (Rgba{250, 0, 0, 255}));
        QCOMPARE(at(now, 39, 29), (Rgba{250, 0, 0, 255}));
    }

    void aPlainLayerStartsFromItsContentBoundsAndTheLayerMaskGoesAlong() {
        CanvasStore s = store();
        PaintDocument d(60, 40);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(60, 40, {1, 2, 3, 255}));
        std::vector<uint8_t> dotPixels(60 * 40 * 4, 0);
        for (int y = 10; y < 20; ++y)
            for (int x = 10; x < 30; ++x)
                for (int k = 0; k < 4; ++k)
                    dotPixels[(size_t(y) * 60 + size_t(x)) * 4 + size_t(k)] = k == 0 ? 200 : k == 3 ? 255 : 0; // x 10..29, y 10..19
        const MaskPaint dot(60, 40, dotPixels, 4);
        std::vector<uint8_t> maskPixels(60 * 40, 255);
        for (int y = 10; y < 20; ++y)
            for (int x = 10; x < 20; ++x)
                maskPixels[size_t(y) * 60 + size_t(x)] = 0; // the left half of the shape is hidden
        LayerInfo info;
        info.name = "Dot";
        const MaskPaint mask(60, 40, maskPixels, 1);
        d.loadLayer(info, dot, &mask);
        const CanvasInfo c = s.create("m", d, "test").value();
        const Tree tree = s.loadTree(c.id, c.head).value();
        const CurrentPlacement now = currentPlacement(tree.layers[1], 60, 40, reader(s)).value();
        QVERIFY(!now.original);
        QCOMPARE(now.sourceWidth, 20);
        QCOMPARE(now.sourceHeight, 10);
        QCOMPARE(now.transform.cx, 20.0);
        QCOMPARE(now.transform.cy, 15.0);
        // Move it 20 px right and double it: the mask goes with it.
        LayerTransform t = now.transform;
        t.cx = 40;
        t.width = 40;
        t.height = 20;
        const Commit done = s.transformLayer(c.id, c.head, 1, t, "canvas-view:transform").value();
        const PaintDocument after = s.load(c.id, done.revision.id).value();
        const MaskPaint layer = *after.pixels(after.layer(1).id);
        const MaskPaint grown = *after.maskPixels(after.layer(1).id);
        // The picture now spans x 20..59, y 5..24; its left half (x 20..39) is hidden by the transformed mask.
        QCOMPARE(at(layer, 50, 15), (Rgba{200, 0, 0, 255}));
        QCOMPARE(at(layer, 19, 15).a, uint8_t(0));
        QCOMPARE(int(grown.pixels()[size_t(15) * 60 + 25]), 0);
        QCOMPARE(int(grown.pixels()[size_t(15) * 60 + 55]), 255);
        QCOMPARE(int(grown.pixels()[size_t(15) * 60 + 5]), 255); // where the layer has nothing the mask hides nothing
        QVERIFY(s.loadTree(c.id, done.revision.id)->layers[1].source->hasMask);
        // A second transform starts from the original (a mask half-hidden at the original's own size), not from the grown one.
        LayerTransform again = t;
        again.width = 20;
        again.height = 10;
        again.cx = 20;
        again.cy = 15;
        const Commit back = s.transformLayer(c.id, done.revision.id, 1, again, "canvas-view:transform").value();
        QVERIFY(same(shown(s, c.id, back.revision.id, 1), dot));
    }

    void aLayerPaintedOnSinceStartsAgainFromItsPixels() {
        CanvasStore s = store();
        const CanvasInfo c = made(s);
        const Commit pasted = s.pasteLayer(c.id, c.head, solid(20, 20, {5, 5, 5, 255}), 50, 40, -1, {}, "p").value();
        Tree t = s.loadTree(c.id, pasted.revision.id).value();
        QVERIFY(hasCurrentOriginal(t.layers[1]));
        const Result<Commit> maskedDone = s.setLayerMask(c.id, pasted.revision.id, 1, MaskPaint(100, 80, std::vector<uint8_t>(100 * 80, 128), 1), "m"); // a layer given a mask is not what the original made
        QVERIFY2(maskedDone.ok(), qPrintable(maskedDone.error().message));
        const Commit masked = maskedDone.value();
        t = s.loadTree(c.id, masked.revision.id).value();
        QVERIFY(!hasCurrentOriginal(t.layers[1]));
        const Result<CurrentPlacement> restart = currentPlacement(t.layers[1], 100, 80, reader(s));
        QVERIFY2(restart.ok(), qPrintable(restart.error().message));
        QVERIFY(!restart->original);
    }

    void groupsMaskAndPictureLayersAndEmptyLayersAreRefusedInWords() {
        CanvasStore s = store();
        PaintDocument d(40, 30);
        d.loadLayer(LayerKind::Paint, "A", true, 1, solid(40, 30, {1, 1, 1, 255}));
        d.loadLayer(LayerKind::Paint, "Empty", true, 1, MaskPaint(40, 30, 4));
        d.loadLayer(LayerKind::Mask, "M", true, 1, MaskPaint(40, 30, 1));
        d.addLayer(LayerKind::Picture, "Pic");
        const CanvasInfo c = s.create("r", d, "test").value();
        const LayerTransform any = place(40, 30, 20, 15, 10, 10);
        QVERIFY(s.transformLayer(c.id, c.head, 1, any, "t").error().message.contains("no pixels"));
        QVERIFY(s.transformLayer(c.id, c.head, 2, any, "t").error().message.contains("mask layer"));
        QVERIFY(s.transformLayer(c.id, c.head, 3, any, "t").error().message.contains("picture layer"));
        QVERIFY(s.transformLayer(c.id, c.head, 9, any, "t").error().message.contains("no layer"));
        LayerEdit group;
        group.op = LayerOp::Group;
        group.layers = {0};
        const Commit grouped = s.editLayers(c.id, c.head, group, "g").value();
        const QString why = s.transformLayer(c.id, grouped.revision.id, 0, any, "t").error().message;
        QVERIFY2(why.contains("group"), qPrintable(why));
        QCOMPARE(s.info(c.id)->head, grouped.revision.id); // nothing was written by a refusal
        QVERIFY(!s.transformLayer(c.id, grouped.revision.id, 0, place(4, 4, 0, 0, 0, 0), "t")); // bad numbers too
    }

    // Growing the canvas draws the layer again from its original, so the part that stuck out of the canvas shows.
    void aCanvasGrownRevealsThePartOfAPictureThatStuckOut() {
        CanvasStore s = store();
        const CanvasInfo c = made(s, 100, 80);
        const MaskPaint big = ramp(200, 150); // pixel (x, y) is (x * 10, y * 10, 7)
        const Commit pasted = s.pasteLayer(c.id, c.head, big, 50, 40, -1, {}, "p").value(); // its corner is at (-50, -35)
        QCOMPARE(at(shown(s, c.id, pasted.revision.id, 1), 0, 0), (Rgba{uint8_t(50 * 10 % 256), uint8_t(35 * 10 % 256), 7, 255})); // picture pixel (50, 35)
        const double before = s.loadTree(c.id, pasted.revision.id)->layers[1].source->transform.cx;
        const Commit grown = s.resize(c.id, pasted.revision.id, IRect{-50, -35, 200, 150}, edgefill::Mode::Transparent, 0, "canvas-view:resize").value();
        const Tree t = s.loadTree(c.id, grown.revision.id).value();
        QVERIFY(hasCurrentOriginal(t.layers[1]));
        QCOMPARE(t.layers[1].source->transform.cx, before + 50);
        const MaskPaint now = shown(s, c.id, grown.revision.id, 1);
        QCOMPARE(now.width(), 200);
        QCOMPARE(at(now, 0, 0), (Rgba{0, 0, 7, 255}));     // picture pixel (0, 0): was off the canvas
        QCOMPARE(at(now, 199, 149).a, uint8_t(255));        // and its far corner
        QCOMPARE(at(now, 60, 40), at(big, 60, 40));
        // The same placement again is the layer as it is: nothing to write; a different one starts from the original.
        QVERIFY(!s.transformLayer(c.id, grown.revision.id, 1, t.layers[1].source->transform, "t")->move.moved);
        // Shrinking the canvas and growing it back keeps the picture whole too.
        const Commit cut = s.resize(c.id, grown.revision.id, IRect{60, 40, 40, 30}, edgefill::Mode::Transparent, 0, "canvas-view:resize").value();
        const Commit again = s.resize(c.id, cut.revision.id, IRect{-60, -40, 200, 150}, edgefill::Mode::Transparent, 0, "canvas-view:resize").value();
        QVERIFY(same(shown(s, c.id, again.revision.id, 1), now));
    }

    void thePlacementsAreStoredToAThousandthSoEqualOnesAreEqual() {
        LayerTransform a = place(1, 1, 10.00049, 5, 20.0004, 8);
        LayerTransform b = place(1, 1, 10.0, 5, 20.0, 8);
        QVERIFY(a.normalized() == b.normalized());
        QCOMPARE(place(1, 1, 0, 0, 4, 4, 270).normalized().angle, -90.0);
        QCOMPARE(place(1, 1, 0, 0, 4, 4, 180).normalized().angle, 180.0);
    }
};

QTEST_GUILESS_MAIN(TestCanvasTransform)
#include "test_canvas_transform.moc"
