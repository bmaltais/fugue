// Growing, shifting and cutting a canvas (#240; docs/inpaint-outpaint-research.md G1) and the edge fill it
// shares with Pad and Extend Edges (#243): engine/edge_fill.h, engine/canvas_resize.h and CanvasStore::resize.
//
// Seam: the pure functions on pixels, and the store with a folder and a clock of the test's own.

#include <QTemporaryDir>
#include <QtTest>

#include "engine/canvas_resize.h"
#include "engine/canvas_store.h"
#include "engine/edge_fill.h"

using namespace fugue;
using namespace fugue::canvas;
using namespace fugue::mask;

namespace {

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

// 4 x 2 red / blue halves, opaque.
edgefill::Image halves() {
    edgefill::Image image{4, 2, 3, {}};
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x) {
            image.data.push_back(x < 2 ? 1.0f : 0.0f);
            image.data.push_back(0.0f);
            image.data.push_back(x < 2 ? 0.0f : 1.0f);
        }
    return image;
}

edgefill::Image padded(const edgefill::Image &in, int left, int top, int right, int bottom, std::vector<uint8_t> *hole) {
    edgefill::Image out{in.width + left + right, in.height + top + bottom, in.channels, {}};
    out.data.assign(size_t(out.width) * out.height * out.channels, 0.25f);
    hole->assign(size_t(out.width) * out.height, 1);
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x) {
            (*hole)[size_t(y + top) * out.width + size_t(x + left)] = 0;
            for (int c = 0; c < in.channels; ++c)
                out.data[(size_t(y + top) * out.width + size_t(x + left)) * out.channels + size_t(c)] = in.data[(size_t(y) * in.width + size_t(x)) * in.channels + size_t(c)];
        }
    return out;
}

} // namespace

class TestCanvasResize : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

private slots:
    void modeNamesRoundTrip() {
        for (const char *name : {"gray", "edge", "edge-blur", "transparent"}) {
            edgefill::Mode mode{};
            QVERIFY(edgefill::modeFromName(QString::fromLatin1(name), &mode));
            QCOMPARE(QString::fromLatin1(edgefill::modeName(mode)), QString::fromLatin1(name));
        }
        edgefill::Mode mode{};
        QVERIFY(!edgefill::modeFromName("blur", &mode));
    }

    void edgeStretchesTheNearestPixelAndLeavesThePictureAlone() {
        std::vector<uint8_t> hole;
        const edgefill::Image original = halves();
        edgefill::Image image = padded(original, 3, 2, 3, 1, &hole);
        const edgefill::Image before = image;
        edgefill::fillHole(image, hole, edgefill::Mode::Edge);
        const auto at = [&](int x, int y, int c) { return image.data[(size_t(y) * image.width + size_t(x)) * 3 + size_t(c)]; };
        QCOMPARE(at(0, 3, 0), 1.0f);  // left of the red half: red
        QCOMPARE(at(9, 3, 2), 1.0f);  // right of the blue half: blue
        QCOMPARE(at(0, 0, 0), 1.0f);  // the top-left corner takes the corner pixel
        QCOMPARE(at(9, 0, 2), 1.0f);  // the top-right corner
        QCOMPARE(at(5, 0, 0), 0.0f);  // above the blue half
        QCOMPARE(at(3, 4, 0), 1.0f);  // below the red half
        for (size_t i = 0; i < hole.size(); ++i)
            if (!hole[i])
                for (int c = 0; c < 3; ++c)
                    QCOMPARE(image.data[i * 3 + size_t(c)], before.data[i * 3 + size_t(c)]); // the picture is not touched
    }

    void edgeBlurSoftensTheStripesButNotThePicture() {
        std::vector<uint8_t> hole;
        edgefill::Image image = padded(halves(), 0, 6, 0, 0, &hole);
        const edgefill::Image before = image;
        edgefill::fillHole(image, hole, edgefill::Mode::EdgeBlur, 4);
        const auto at = [&](int x, int y, int c) { return image.data[(size_t(y) * image.width + size_t(x)) * 3 + size_t(c)]; };
        QVERIFY(at(1, 0, 0) < 1.0f && at(1, 0, 0) > 0.5f); // red, bled towards the blue
        QVERIFY(at(2, 0, 0) > 0.0f && at(2, 0, 0) < 0.5f);
        QVERIFY(at(1, 0, 0) > at(2, 0, 0));
        for (size_t i = 0; i < hole.size(); ++i)
            if (!hole[i])
                for (int c = 0; c < 3; ++c)
                    QCOMPARE(image.data[i * 3 + size_t(c)], before.data[i * 3 + size_t(c)]);
        // A blur radius of 0 is Edge.
        edgefill::Image sharp = before, edge = before;
        edgefill::fillHole(sharp, hole, edgefill::Mode::EdgeBlur, 0);
        edgefill::fillHole(edge, hole, edgefill::Mode::Edge);
        QCOMPARE(sharp.data, edge.data);
    }

    void grayAndTransparent() {
        std::vector<uint8_t> hole;
        edgefill::Image rgba{2, 1, 4, {1, 0, 0, 1, 0.1f, 0.2f, 0.3f, 0.4f}};
        hole = {0, 1};
        edgefill::Image gray = rgba;
        edgefill::fillHole(gray, hole, edgefill::Mode::Gray);
        QCOMPARE(gray.data[4], 0.5f);
        QCOMPARE(gray.data[7], 1.0f);
        edgefill::Image clear = rgba;
        edgefill::fillHole(clear, hole, edgefill::Mode::Transparent);
        QCOMPARE(clear.data[7], 0.0f);
        QCOMPARE(clear.data[0], 1.0f);
        edgefill::Image rgb{2, 1, 3, {1, 0, 0, 0.1f, 0.2f, 0.3f}};
        edgefill::fillHole(rgb, hole, edgefill::Mode::Transparent); // no alpha: gray
        QCOMPARE(rgb.data[3], 0.5f);
    }

    void aHoleWithNothingAroundItIsGray() {
        edgefill::Image image{2, 2, 3, std::vector<float>(12, 0.0f)};
        edgefill::fillHole(image, std::vector<uint8_t>(4, 1), edgefill::Mode::EdgeBlur);
        for (float v : image.data)
            QCOMPARE(v, 0.5f);
    }

    void aTransparentPixelDoesNotBleedItsColour() {
        // An opaque red pixel beside a transparent one that holds green: the blur must not turn red green.
        std::vector<uint8_t> hole(8, 0);
        edgefill::Image image{2, 1, 4, {1, 0, 0, 1, 0, 1, 0, 0}};
        edgefill::blurImage(image, 3);
        QVERIFY(image.data[1] < 0.01f); // still no green in the opaque pixel
    }

    void nearestKeptFindsTheNearest() {
        std::vector<uint8_t> hole(5 * 5, 1);
        hole[2 * 5 + 2] = 0;
        const std::vector<int> nearest = edgefill::nearestKept(5, 5, hole);
        for (int index : nearest)
            QCOMPARE(index, 2 * 5 + 2);
        QCOMPARE(edgefill::nearestKept(2, 1, {1, 1}), (std::vector<int>{-1, -1}));
    }

    void growingMovesLayersAndPreFillsTheNewArea() {
        PaintDocument d(8, 8);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(8, 8, {200, 40, 40, 255}));
        MaskPaint dot(8, 8, 4);
        dot.fill({0, 0, 255, 255});
        d.loadLayer(LayerKind::Paint, "Dot", true, 0.5, dot);
        const Result<PaintDocument> grown = resizedCanvas(d, IRect{-4, -2, 16, 12}, edgefill::Mode::Edge);
        QVERIFY(grown.ok());
        QCOMPARE(grown->width(), 16);
        QCOMPARE(grown->height(), 12);
        QCOMPARE(grown->layerCount(), 3);
        QCOMPARE(grown->layer(0).name, QString(resizeFillLayerName));
        QCOMPARE(grown->layer(0).producer, QString(resizeProducer));
        QCOMPARE(grown->layer(1).name, QString("Base"));
        QCOMPARE(grown->layer(2).opacity, 0.5);
        const MaskPaint *base = grown->pixels(grown->layer(1).id);
        QCOMPARE(base->rgbaAt(4, 2).r, uint8_t(200));  // the old (0, 0) is now at (4, 2)
        QCOMPARE(base->rgbaAt(3, 2).a, uint8_t(0));    // nothing beyond it
        QCOMPARE(base->rgbaAt(11, 9).a, uint8_t(255)); // the old (7, 7)
        const MaskPaint *fill = grown->pixels(grown->layer(0).id);
        QCOMPARE(fill->rgbaAt(0, 0).r, uint8_t(100));  // the corner takes the flattened corner: the blue dot at half over red
        QCOMPARE(fill->rgbaAt(0, 0).a, uint8_t(255));
        QCOMPARE(fill->rgbaAt(6, 4).a, uint8_t(0));    // inside the old canvas the fill layer is transparent
        const MaskPaint flat = grown->flatten();
        QCOMPARE(flat.rgbaAt(0, 5).a, uint8_t(255));    // the whole new canvas is covered
        QCOMPARE(flat.rgbaAt(15, 11).a, uint8_t(255));
    }

    void transparentFillAddsNoLayer() {
        PaintDocument d(8, 8);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(8, 8, {200, 40, 40, 255}));
        const Result<PaintDocument> grown = resizedCanvas(d, IRect{0, 0, 12, 8}, edgefill::Mode::Transparent);
        QVERIFY(grown);
        QCOMPARE(grown->layerCount(), 1);
        QCOMPARE(grown->flatten().rgbaAt(10, 3).a, uint8_t(0));
    }

    void cuttingNeedsNoFillAndKeepsMasks() {
        PaintDocument d(8, 8);
        const MaskPaint layerMask(8, 8, std::vector<uint8_t>(64, 100), 1);
        LayerInfo info;
        info.name = "Masked";
        d.loadLayer(info, solid(8, 8, {1, 2, 3, 255}), &layerMask);
        d.loadLayer(LayerKind::Mask, "Sel", true, 1, MaskPaint(8, 8, std::vector<uint8_t>(64, 77), 1));
        const Result<PaintDocument> cut = resizedCanvas(d, IRect{2, 2, 4, 3}, edgefill::Mode::EdgeBlur);
        QVERIFY(cut);
        QCOMPARE(cut->layerCount(), 2);
        QCOMPARE(cut->width(), 4);
        QCOMPARE(cut->height(), 3);
        QVERIFY(cut->layer(0).hasMask);
        QCOMPARE(cut->maskPixels(cut->layer(0).id)->at(1, 1), uint8_t(100));
        QCOMPARE(cut->pixels(cut->layer(1).id)->at(3, 2), uint8_t(77));
        QCOMPARE(cut->pixels(cut->layer(1).id)->channels(), 1);
    }

    void aLayerMaskIsWhiteInTheNewArea() {
        PaintDocument d(4, 4);
        const MaskPaint layerMask(4, 4, std::vector<uint8_t>(16, 0), 1);
        LayerInfo info;
        info.name = "Masked";
        d.loadLayer(info, solid(4, 4, {1, 2, 3, 255}), &layerMask);
        const Result<PaintDocument> grown = resizedCanvas(d, IRect{0, 0, 6, 4}, edgefill::Mode::Transparent);
        QVERIFY(grown);
        QCOMPARE(grown->maskPixels(grown->layer(0).id)->at(1, 1), uint8_t(0));
        QCOMPARE(grown->maskPixels(grown->layer(0).id)->at(5, 1), uint8_t(255));
    }

    void aRectangleThatHoldsNothingOrIsTooBigIsRefused() {
        PaintDocument d(8, 8);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(8, 8, {1, 2, 3, 255}));
        for (const IRect &bad : {IRect{8, 0, 4, 4}, IRect{-4, 0, 4, 4}, IRect{0, 0, 0, 4}, IRect{0, 0, 20000, 4}}) {
            const Result<PaintDocument> r = resizedCanvas(d, bad, edgefill::Mode::Edge);
            QVERIFY(!r);
            QCOMPARE(r.errors().first().code, QStringLiteral("input_invalid"));
        }
    }

    void theStoreMakesOneRevisionThatUndoTakesBack() {
        CanvasStore s(m_dir.path());
        PaintDocument d(300, 200);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(300, 200, {10, 20, 30, 255}));
        const CanvasInfo made = s.create("Grow", d, "new").value();
        const Result<Commit> done = s.resize(made.id, made.head, IRect{-100, 0, 500, 200}, edgefill::Mode::EdgeBlur, 24, "canvas-cli:resize");
        QVERIFY(done.ok());
        QVERIFY(done->move.moved);
        QCOMPARE(done->revision.parent, made.head);
        QCOMPARE(done->revision.producer, QStringLiteral("canvas-cli:resize"));
        const Tree tree = s.tree(done->revision.tree).value();
        QCOMPARE(tree.width, 500);
        QCOMPARE(tree.height, 200);
        QCOMPARE(int(tree.layers.size()), 2);
        QCOMPARE(tree.layers[0].name, QString(resizeFillLayerName));
        const PaintDocument loaded = s.load(made.id, done->revision.id).value();
        QCOMPARE(loaded.flatten().rgbaAt(10, 100).r, uint8_t(10));  // the pre-fill continues the colour
        QCOMPARE(loaded.flatten().rgbaAt(150, 100).b, uint8_t(30)); // the old picture, moved right by 100
        // The head was the one expected: a second call with the old head changes nothing.
        const Result<Commit> stale = s.resize(made.id, made.head, IRect{0, 0, 10, 10}, edgefill::Mode::Edge, 0, "x");
        QVERIFY(stale);
        QVERIFY(!stale->move.moved);
        QCOMPARE(stale->move.head, done->revision.id);
        QVERIFY(s.undo(made.id, done->revision.id)->moved);
        QCOMPARE(s.info(made.id)->head, made.head);
    }
};

QTEST_GUILESS_MAIN(TestCanvasResize)
#include "test_canvas_resize.moc"
