// Canvas Region and Canvas Write (#209, epic #206, spec 14.5).
//
// Seams: the geometry (nodes/canvas_region_ops.h) on float buffers; each node's kernel run as the
// scheduler runs it, against the session's canvas store (the test's own home); and a workflow run
// through compileForRun and the real scheduler for what only the engine can show: that the revision
// is named once, that the same revision gives the same bytes, that a re-run of the compiled graph is a
// cache hit which writes nothing twice, and that a Write moves the head by compare-and-swap.

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <vector>

#include "engine/canvas_handle.h"
#include "engine/canvas_layer_ops.h"
#include "engine/canvas_store.h"
#include "engine/canvas_tiles.h"
#include "engine/compile_for_run.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/registry.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"
#include "engine/scheduler.h"
#include "nodes/builtin.h"
#include "nodes/canvas_region_ops.h"
#include "nodes/kernels.h"
#include "nodes/pixel_io.h"

using namespace fugue;
using namespace fugue::nodes;
using fugue::canvas::CanvasStore;
using fugue::mask::LayerKind;
using fugue::mask::PaintDocument;
using fugue::mask::Rgba;

namespace {

constexpr int kW = 400, kH = 300;

// A canvas with two colored blocks on its base layer: red at (50,40) 120x90 and blue at (250,150) 100x100.
PaintDocument sample() {
    PaintDocument doc(kW, kH);
    doc.addLayer(LayerKind::Paint, "Base");
    doc.brush.color = Rgba{200, 30, 30, 255};
    doc.fillRect(50, 40, 120, 90);
    doc.brush.color = Rgba{20, 40, 220, 255};
    doc.fillRect(250, 150, 100, 100);
    return doc;
}

QString makeCanvas() {
    const auto made = CanvasStore::forSession().create("Test", sample(), "test");
    if (!made)
        qFatal("cannot make a canvas");
    return made->id;
}

QString headOf(const QString &id) { return CanvasStore::forSession().info(id)->head; }

// The straight RGBA byte at x, y, channel of a revision's composite.
int pixel(const QString &id, const QString &revision, int x, int y, int channel) {
    const auto doc = CanvasStore::forSession().load(id, revision);
    if (!doc)
        qFatal("cannot load a revision");
    return doc->flatten().pixels()[(size_t(y) * doc->width() + x) * 4 + channel];
}

Pixels solid(int w, int h, float r, float g, float b) {
    Pixels p(w, h, 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            p.at(x, y, 0) = r;
            p.at(x, y, 1) = g;
            p.at(x, y, 2) = b;
        }
    return p;
}

// Kernels run as the scheduler runs them, each node in a work folder of its own.
class Bench {
public:
    Bench() : hashes(dir.filePath("hashes.json")) {
        nodes::addBuiltinNodeTypes(registry);
        nodes::addBuiltinKernels(kernels);
    }
    NodeContext context(const QString &id, Values inputs, const QString &work) {
        const NodeType &type = *registry.nodeType(id);
        for (const InputSpec &spec : type.inputs)
            if (!inputs.contains(spec.name) && !spec.defaultValue.isUndefined())
                inputs.insert(spec.name, Value(spec.defaultValue));
        NodeContext c;
        c.node = work;
        c.type = &type;
        c.inputs = inputs;
        c.workDir = dir.filePath(work);
        QDir().mkpath(c.workDir);
        c.progress = [](double, const QString &, const QString &) {};
        c.cancelled = [] { return false; };
        c.hashes = &hashes;
        c.registry = &registry;
        return c;
    }
    Result<Values> run(const QString &id, const Values &inputs, const QString &work, const QJsonObject &document = {}) {
        NodeContext c = context(id, inputs, work);
        c.document = document;
        return kernels.node(id)->run(c);
    }
    // Canvas Region on the head of `id`, the revision named in full as a compile does.
    Result<Values> region(const QString &id, Values inputs = {}, const QString &work = "region") {
        inputs.insert("canvas", Value(id));
        if (!inputs.contains("revision"))
            inputs.insert("revision", Value(headOf(id)));
        return run("fugue/canvas-region", inputs, work);
    }

    QTemporaryDir dir;
    FileHashes hashes;
    Registry registry;
    Kernels kernels;
};

const NodeType &registryNode(Bench &bench, const QString &id) { return *bench.registry.nodeType(id); }

QString codeOf(const Result<Values> &r) { return r.errors().isEmpty() ? QString() : r.errors().first().code; }

} // namespace

class TestCanvasNodes : public QObject {
    Q_OBJECT

private slots:
    // ---- geometry ----

    void aRectangleIsCutToTheCanvasAndZeroRunsToTheEdge() {
        QCOMPARE(*rectBox(10, 20, 30, 40, 100, 100), (CropBox{10, 20, 30, 40}));
        QCOMPARE(*rectBox(90, 90, 30, 40, 100, 100), (CropBox{90, 90, 10, 10}));
        QCOMPARE(*rectBox(10, 20, 0, 0, 100, 80), (CropBox{10, 20, 90, 60}));
        QCOMPARE(*rectBox(0, 0, 0, 0, 100, 80), (CropBox{0, 0, 100, 80}));
        QVERIFY(!rectBox(100, 0, 5, 5, 100, 100));
        QVERIFY(!rectBox(0, 100, 5, 5, 100, 100));
    }

    void aBoxMaskIsOneInsideTheBoxAndZeroOutside() {
        const Pixels m = boxMask({2, 3, 4, 2}, 10, 8);
        QCOMPARE(m.channels, 1);
        float sum = 0;
        for (float v : m.data)
            sum += v;
        QCOMPARE(sum, 8.0f);
        QCOMPARE(m.at(2, 3, 0), 1.0f);
        QCOMPARE(m.at(5, 4, 0), 1.0f);
        QCOMPARE(m.at(6, 4, 0), 0.0f);
        QCOMPARE(m.at(2, 5, 0), 0.0f);
    }

    void theRegionSizeFollowsScaleCapAndMultiple_data() {
        QTest::addColumn<CropBox>("box");
        QTest::addColumn<double>("scale");
        QTest::addColumn<double>("cap");
        QTest::addColumn<int>("multiple");
        QTest::addColumn<CropSize>("want");
        QTest::newRow("as is") << CropBox{0, 0, 320, 160} << 1.0 << 0.0 << 16 << CropSize{320, 160};
        QTest::newRow("doubled") << CropBox{0, 0, 320, 160} << 2.0 << 0.0 << 16 << CropSize{640, 320};
        QTest::newRow("snapped to the step") << CropBox{0, 0, 100, 100} << 1.0 << 0.0 << 16 << CropSize{96, 96};
        QTest::newRow("one step at least") << CropBox{0, 0, 5, 5} << 1.0 << 0.0 << 16 << CropSize{16, 16};
        QTest::newRow("step 1 is exact") << CropBox{0, 0, 101, 53} << 1.0 << 0.0 << 1 << CropSize{101, 53};
        // 2000 x 1000 at scale 1 is 2 MP: capped at 0.5 MP keeping 2:1, rounded down so the cap holds.
        QTest::newRow("capped") << CropBox{0, 0, 2000, 1000} << 1.0 << 0.5 << 16 << CropSize{992, 496};
        QTest::newRow("the cap bites after the scale") << CropBox{0, 0, 1000, 1000} << 4.0 << 1.0 << 16 << CropSize{992, 992};
        QTest::newRow("no side past the limit") << CropBox{0, 0, 16000, 100} << 1.0 << 0.0 << 16 << CropSize{4096, 16};
    }

    void aTargetBringsASmallRegionUpToTheModelsSizeAndDownFromALargeOne() {
        // Crop to Mask's rule: 432 x 464 is about 0.2 MP, brought up to about 1 MP in its shape (#242).
        const CropBox small{0, 0, 432, 464};
        const CropSize up = regionSize(small, 1, 0, 16, 1.0);
        QCOMPARE(up, cropSizeFor(small, 1.0, 16)); // the same function, not a copy of its rule
        QVERIFY(up.width * up.height > 900000);
        const CropBox large{0, 0, 3000, 2000};
        QCOMPARE(regionSize(large, 1, 0, 16, 1.0), cropSizeFor(large, 1.0, 16));
        QVERIFY(regionSize(large, 1, 0, 16, 1.0).width < 3000);
        // The scale is not used with a target; the cap still only brings it down; 0 is off.
        QCOMPARE(regionSize(small, 4, 0, 16, 1.0), up);
        const CropSize capped = regionSize(small, 1, 0.5, 16, 1.0);
        QVERIFY(double(capped.width) * capped.height <= 0.5e6);
        QCOMPARE(regionSize(small, 2, 0, 16, 0), regionSize(small, 2, 0, 16));
    }

    void thePaddingHoldsRoomForTheFeatherAndTheBlend() {
        QCOMPARE(paddingFor(0, 0), 16);
        QCOMPARE(paddingFor(64, 0), 64);
        QCOMPARE(paddingFor(20, 32), 48);
        QCOMPARE(paddingFor(100, 32), 100);
    }
    void theRegionSizeFollowsScaleCapAndMultiple() {
        QFETCH(CropBox, box);
        QFETCH(double, scale);
        QFETCH(double, cap);
        QFETCH(int, multiple);
        QFETCH(CropSize, want);
        const CropSize got = regionSize(box, scale, cap, multiple);
        QCOMPARE(got.width, want.width);
        QCOMPARE(got.height, want.height);
        if (cap > 0)
            QVERIFY(double(got.width) * got.height <= cap * 1e6);
    }

    void aPlacedLayerHoldsOnlyTheBoxAndFadesAtItsEdge() {
        // A 40 x 30 canvas, a 20 x 10 box at (10, 10), the mask the middle 10 x 4 of it.
        const CropBox box{10, 10, 20, 10};
        Pixels mask(40, 30, 1);
        for (int y = 13; y < 17; ++y)
            for (int x = 15; x < 25; ++x)
                mask.at(x, y, 0) = 1;
        const Pixels redrawn = solid(20, 10, 1, 0.5f, 0.25f);
        const Pixels hard = placedLayer(40, 30, box, redrawn, mask, 0);
        QCOMPARE(hard.channels, 4);
        QCOMPARE(hard.at(20, 14, 3), 1.0f);
        QCOMPARE(hard.at(20, 14, 1), 0.5f);
        QCOMPARE(hard.at(14, 14, 3), 0.0f);
        QCOMPARE(hard.at(14, 14, 0), 0.0f); // no color where nothing is placed
        const Pixels soft = placedLayer(40, 30, box, redrawn, mask, 3);
        QVERIFY(soft.at(14, 14, 3) > 0.0f && soft.at(14, 14, 3) < 1.0f); // fades outward from the mask
        QCOMPARE(soft.at(20, 14, 3), 1.0f);
        QCOMPARE(soft.at(5, 14, 3), 0.0f);  // outside the box
        QCOMPARE(soft.at(20, 25, 3), 0.0f);
        for (int x = 0; x < 40; ++x)
            QCOMPARE(soft.at(x, 5, 3), 0.0f);
    }

    void aPlacedLayerTakesTheRedrawnPicturesAlphaToo() {
        Pixels redrawn(4, 4, 4, 1.0f);
        for (int x = 0; x < 4; ++x)
            redrawn.at(x, 0, 3) = 0.5f;
        const Pixels layer = placedLayer(4, 4, {0, 0, 4, 4}, redrawn, Pixels(4, 4, 1, 1.0f), 0);
        QCOMPARE(layer.at(1, 0, 3), 0.5f);
        QCOMPARE(layer.at(1, 1, 3), 1.0f);
    }


    // The layer and its mask kept apart (#226) composite to the baked layer, within rounding.
    void aLayerWithItsMaskCompositesToTheBakedLayer() {
        const CropBox box{10, 10, 20, 10};
        Pixels mask(40, 30, 1);
        for (int y = 13; y < 17; ++y)
            for (int x = 15; x < 25; ++x)
                mask.at(x, y, 0) = 1;
        Pixels redrawn(20, 10, 4, 0.6f);
        for (int x = 0; x < 20; ++x)
            redrawn.at(x, 2, 3) = 0.5f;
        const Pixels baked = placedLayer(40, 30, box, redrawn, mask, 3);
        const PlacedLayer apart = placedLayerWithMask(40, 30, box, redrawn, mask, 3);
        QCOMPARE(apart.mask.channels, 1);
        QCOMPARE(apart.layer.at(10, 10, 3), 0.6f);   // the picture whole, where the mask hides it
        QCOMPARE(apart.mask.at(10, 10, 0), 0.0f);
        QCOMPARE(apart.layer.at(5, 5, 3), 0.0f);     // nothing outside the box
        for (int y = 0; y < 30; ++y)
            for (int x = 0; x < 40; ++x)
                QVERIFY(std::abs(apart.layer.at(x, y, 3) * apart.mask.at(x, y, 0) - baked.at(x, y, 3)) < 0.5f / 255.0f + 1e-6f); // the baked one drops alpha under half a byte
    }

    // ---- Canvas Region ----

    void aRegionCutsTheRectangleOfTheRevision() {
        Bench bench;
        const QString id = makeCanvas();
        const Result<Values> r = bench.region(id, {{"x", Value(40)}, {"y", Value(30)}, {"width", Value(150)}, {"height", Value(120)},
                                                   {"padding", Value(16)}, {"scale", Value(1)}, {"max_megapixels", Value(0)}, {"multiple", Value(1)}});
        QVERIFY2(r.ok(), qPrintable(codeOf(r)));
        QCOMPARE(r->value("width").number(), 182.0); // 150 + 2 x 16 (the least padding: room for the blend)
        QCOMPARE(r->value("height").number(), 152.0);
        const Pixels image = pixelsOf(r->value("image"), "image").value();
        QCOMPARE(image.width, 182);
        QCOMPARE(image.height, 152);
        QCOMPARE(image.channels, 4);
        // (50,40) is the red block's corner: at (10,10) in the region. (0,0) is empty canvas.
        QCOMPARE(image.at(26, 26, 0), 200 / 255.0f);
        QCOMPARE(image.at(26, 26, 3), 1.0f);
        QCOMPARE(image.at(0, 0, 3), 0.0f);
        const Pixels mask = maskOf(r->value("mask"), "mask").value();
        QCOMPARE(mask.at(75, 60, 0), 1.0f); // the rectangle is all of it with the least padding
        // The handle says which canvas and revision was read.
        const auto handle = canvas::canvasHandleFrom(r->value("canvas").json());
        QVERIFY(handle);
        QCOMPARE(handle->canvas, id);
        QCOMPARE(handle->revision, headOf(id));
    }

    // The Harmonize action (a region, then a seam map from the region's mask and size): picture, mask and the width and
    // height outputs agree at every scale and cap, so the VAE Encode never sees a mask of another size.
    void theRegionsPictureMaskAndSizeAgreeWhenScaledAndCapped() {
        Bench bench;
        const QString id = makeCanvas();
        for (const auto &[scale, cap] : {std::pair{1.0, 0.0}, std::pair{2.0, 0.0}, std::pair{4.0, 0.02}}) {
            const Result<Values> r = bench.region(id, {{"x", Value(33)}, {"y", Value(21)}, {"width", Value(111)}, {"height", Value(97)},
                                                       {"padding", Value(16)}, {"scale", Value(scale)}, {"max_megapixels", Value(cap)}, {"multiple", Value(16)}});
            QVERIFY(r.ok());
            const Pixels image = pixelsOf(r->value("image"), "image").value();
            const Pixels mask = maskOf(r->value("mask"), "mask").value();
            QCOMPARE(mask.width, image.width);
            QCOMPARE(mask.height, image.height);
            QCOMPARE(double(image.width), r->value("width").number());
            QCOMPARE(double(image.height), r->value("height").number());
        }
    }

    void paddingMarksTheRectangleInsideTheRegionsMask() {
        Bench bench;
        const QString id = makeCanvas();
        const Result<Values> r = bench.region(id, {{"x", Value(100)}, {"y", Value(100)}, {"width", Value(60)}, {"height", Value(40)},
                                                   {"padding", Value(20)}, {"max_megapixels", Value(0)}, {"multiple", Value(1)}});
        QVERIFY(r.ok());
        QCOMPARE(r->value("width").number(), 100.0);
        QCOMPARE(r->value("height").number(), 80.0);
        const Pixels mask = maskOf(r->value("mask"), "mask").value();
        QCOMPARE(mask.at(5, 5, 0), 0.0f);   // padding
        QCOMPARE(mask.at(50, 40, 0), 1.0f); // the rectangle
    }

    void aMaskGivesTheRegionItsBounds() {
        Bench bench;
        const QString id = makeCanvas();
        Pixels m(kW, kH, 1);
        for (int y = 160; y < 200; ++y)
            for (int x = 260; x < 300; ++x)
                m.at(x, y, 0) = 1;
        Result<Values> maskValue = namedOutput(m, "mask");
        QVERIFY(maskValue.ok());
        const MediaHandle file = maskValue->value("mask").media().saved(bench.dir.path(), "m", bench.hashes).value();
        const Result<Values> r = bench.region(id, {{"mask", Value(file)}, {"padding", Value(20)}, {"max_megapixels", Value(0)}, {"multiple", Value(1)},
                                                   {"x", Value(0)}, {"width", Value(5)}});
        QVERIFY2(r.ok(), qPrintable(codeOf(r)));
        QCOMPARE(r->value("width").number(), 80.0); // 40 + 2 x 20: the rectangle inputs are not used
        QCOMPARE(r->value("height").number(), 80.0);
    }

    // Region 1.2 (#230): a selection painted in the Canvas view, stored by its hash, is the part to change; the mask and
    // the rectangle are not used, and the hash is a literal (the same selection is a cache hit). Empty, unknown or not a
    // hash: said plainly.
    void aStoredSelectionGivesTheRegionItsBounds() {
        Bench bench;
        const QString id = makeCanvas();
        mask::MaskPaint painted(kW, kH);
        painted.fillRect(100, 60, 40, 30);
        const QString hash = CanvasStore::forSession().storeSelection(painted).value();
        const Result<Values> r = bench.region(id, {{"selection", Value(hash)}, {"padding", Value(20)}, {"max_megapixels", Value(0)}, {"multiple", Value(1)},
                                                   {"x", Value(0)}, {"width", Value(5)}});
        QVERIFY2(r.ok(), qPrintable(codeOf(r)));
        QCOMPARE(r->value("width").number(), 80.0); // 40 + 2 x 20
        QCOMPARE(r->value("height").number(), 70.0);
        QCOMPARE(codeOf(bench.region(id, {{"selection", Value(CanvasStore::forSession().storeSelection(mask::MaskPaint(kW, kH)).value())}})), QString("mask_empty"));
        QCOMPARE(codeOf(bench.region(id, {{"selection", Value("sha256:" + QString(64, '0'))}})), QString("canvas_not_found"));
        QCOMPARE(codeOf(bench.region(id, {{"selection", Value("not a hash")}})), QString("input_invalid"));
        const NodeType &type = *bench.registry.nodeType("fugue/canvas-region");
        QCOMPARE(type.version.toString(), QString("1.4.0"));
    }

    void anEmptyMaskOrARectangleOffTheCanvasIsRefused() {
        Bench bench;
        const QString id = makeCanvas();
        Result<Values> blank = namedOutput(Pixels(kW, kH, 1), "mask");
        const MediaHandle file = blank->value("mask").media().saved(bench.dir.path(), "blank", bench.hashes).value();
        QCOMPARE(codeOf(bench.region(id, {{"mask", Value(file)}})), QString("mask_empty"));
        QCOMPARE(codeOf(bench.region(id, {{"x", Value(500)}, {"width", Value(5)}})), QString("value_out_of_range"));
    }

    void theScaleAndTheCapSetTheSizeTheRegionIsRedrawnAt() {
        Bench bench;
        const QString id = makeCanvas();
        const Values rect{{"x", Value(0)}, {"y", Value(0)}, {"width", Value(200)}, {"height", Value(100)}, {"padding", Value(16)}};
        Values doubled = rect;
        doubled.insert("scale", Value(2));
        doubled.insert("max_megapixels", Value(0));
        const Result<Values> big = bench.region(id, doubled);
        QCOMPARE(big->value("width").number(), 464.0); // (200 + 2 x 16) x 2
        QCOMPARE(big->value("height").number(), 272.0); // (100 + 2 x 16) x 2 = 264, snapped to the nearest multiple of 16
        Values capped = rect;
        capped.insert("scale", Value(4));
        capped.insert("max_megapixels", Value(0.05));
        const Result<Values> small = bench.region(id, capped);
        QVERIFY(small->value("width").number() * small->value("height").number() <= 50000.0);
    }

    void aRegionRedrawnAtATargetSizeAndPaddingRaisedForTheBlend() {
        Bench bench;
        const QString id = makeCanvas();
        const Values rect{{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}, {"max_megapixels", Value(0)}};
        Values fixed = rect;
        fixed.insert("padding", Value(16));
        fixed.insert("target_megapixels", Value(0.25));
        const Result<Values> r = bench.region(id, fixed, "target");
        QVERIFY2(r.ok(), qPrintable(codeOf(r)));
        QVERIFY(r.warnings().isEmpty());
        const double pixels = r->value("width").number() * r->value("height").number();
        QVERIFY(pixels > 0.2e6 && pixels < 0.3e6); // 132 x 112 brought up to about 0.25 MP
        // Padding 0 with a feather: raised to feather + 16 with a warning that names the input.
        Values low = rect;
        low.insert("padding", Value(0));
        low.insert("feather", Value(8));
        low.insert("multiple", Value(1));
        const Result<Values> raised = bench.region(id, low, "raised");
        QVERIFY2(raised.ok(), qPrintable(codeOf(raised)));
        QCOMPARE(raised.warnings().value(0).code, QString("padding_raised"));
        QCOMPARE(raised.warnings().value(0).input, QString("padding"));
        QCOMPARE(raised->value("width").number(), 148.0); // 100 + 2 * 24
        QCOMPARE(raised->value("height").number(), 128.0);
        Values bad = rect;
        bad.insert("target_megapixels", Value(40));
        QCOMPARE(codeOf(bench.region(id, bad, "bad")), QString("value_out_of_range"));
    }

    void theFeatherSoftensTheMaskOutputOnly() {
        Bench bench;
        const QString id = makeCanvas();
        const Values rect{{"x", Value(100)}, {"y", Value(100)}, {"width", Value(60)}, {"height", Value(60)}, {"padding", Value(40)},
                          {"max_megapixels", Value(0)}, {"multiple", Value(1)}};
        const Pixels hard = maskOf(bench.region(id, rect, "hard")->value("mask"), "mask").value();
        Values soft = rect;
        soft.insert("feather", Value(10));
        const Pixels feathered = maskOf(bench.region(id, soft, "soft")->value("mask"), "mask").value();
        QCOMPARE(hard.at(35, 50, 0), 0.0f);
        QVERIFY(feathered.at(35, 50, 0) > 0.0f);  // outside the rectangle: the zone the model may redraw
        QCOMPARE(feathered.at(70, 70, 0), 1.0f);
    }

    void aRevisionThatWasNotNamedInFullIsRefused() {
        Bench bench;
        const QString id = makeCanvas();
        QCOMPARE(codeOf(bench.region(id, {{"revision", Value("head")}})), QString("input_invalid"));
        QCOMPARE(codeOf(bench.run("fugue/canvas-region", {{"canvas", Value("not a canvas")}, {"revision", Value(headOf(id))}}, "bad")),
                 QString("input_invalid"));
    }

    void aMissingRevisionIsCanvasNotFound() {
        Bench bench;
        const QString id = makeCanvas();
        QCOMPARE(codeOf(bench.region(id, {{"revision", Value("sha256:" + QString(64, '1'))}})), QString("canvas_not_found"));
    }

    void theSameRevisionGivesTheSameBytes() {
        Bench bench;
        const QString id = makeCanvas();
        const Values rect{{"x", Value(40)}, {"y", Value(30)}, {"width", Value(150)}, {"height", Value(120)}};
        const Result<Values> a = bench.region(id, rect, "a");
        // The canvas moves on (a Write makes a new head); the first revision still reads the same.
        const QString first = headOf(id);
        auto doc = CanvasStore::forSession().load(id, first);
        doc->loadLayer(LayerKind::Paint, "More", true, 1, mask::MaskPaint(kW, kH, std::vector<uint8_t>(size_t(kW) * kH * 4, 255), 4));
        QVERIFY(CanvasStore::forSession().commit(id, doc.value(), first, "test"));
        QVERIFY(headOf(id) != first);
        Values again = rect;
        again.insert("revision", Value(first));
        const Result<Values> b = bench.region(id, again, "b");
        const Pixels pa = pixelsOf(a->value("image"), "image").value();
        const Pixels pb = pixelsOf(b->value("image"), "image").value();
        QCOMPARE(pa.data, pb.data);
        QCOMPARE(maskOf(a->value("mask"), "mask")->data, maskOf(b->value("mask"), "mask")->data);
        QCOMPARE(a->value("stitch").json().toObject().value("meta").toObject().value("box"),
                 b->value("stitch").json().toObject().value("meta").toObject().value("box"));
        // The newest revision is not the same picture (the white layer covers it).
        const Pixels pc = pixelsOf(bench.region(id, rect, "c")->value("image"), "image").value();
        QVERIFY(pc.data != pa.data);
    }

    void theStitchPutsAnUnchangedRegionBackAsTheCanvas() {
        // Canvas Region's stitch is Stitch's: the region as it came, stitched back, is the canvas.
        Bench bench;
        const QString id = makeCanvas();
        const Result<Values> r = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)},
                                                   {"padding", Value(16)}, {"max_megapixels", Value(0)}, {"multiple", Value(1)}});
        QVERIFY(r.ok());
        const Result<Values> s = bench.run("fugue/stitch", {{"stitch", r->value("stitch")}, {"image", r->value("image")}, {"blend", Value(0)}}, "stitch");
        QVERIFY2(s.ok(), qPrintable(codeOf(s)));
        const Pixels back = pixelsOf(s->value("image"), "image").value();
        const auto doc = CanvasStore::forSession().load(id, headOf(id));
        const Pixels original = layerToPixels(doc->flatten());
        QCOMPARE(back.width, original.width);
        float worst = 0;
        for (size_t i = 0; i < back.data.size(); ++i)
            worst = std::max(worst, std::fabs(back.data[i] - original.data[i]));
        QVERIFY2(worst < 0.01f, qPrintable(QString::number(worst)));
    }

    // ---- Canvas Write ----

    // A region of the red block, the redrawn picture solid green, written back.
    Result<Values> writeGreen(Bench &bench, const Result<Values> &region, Values extra = {}, const QString &work = "write") {
        Result<Values> green = imageOutput(solid(region->value("width").number(), region->value("height").number(), 0, 1, 0));
        Values inputs{{"canvas", region->value("canvas")}, {"image", green->value("image")}, {"stitch", region->value("stitch")}, {"blend", Value(0)}};
        for (auto it = extra.begin(); it != extra.end(); ++it)
            inputs.insert(it.key(), it.value());
        return bench.run("fugue/canvas-write", inputs, work);
    }

    void aWriteAddsALayerAndMovesTheHead() {
        Bench bench;
        const QString id = makeCanvas();
        const QString before = headOf(id);
        const Result<Values> r = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)},
                                                   {"padding", Value(16)}, {"max_megapixels", Value(0)}, {"multiple", Value(1)}});
        const Result<Values> w = writeGreen(bench, r, {{"name", Value("Green")}});
        QVERIFY2(w.ok(), qPrintable(codeOf(w)));
        QVERIFY(w->value("accepted").json().toBool());
        const QString after = headOf(id);
        QVERIFY(after != before);
        QCOMPARE(w->value("head").text(), after);
        const auto handle = canvas::canvasHandleFrom(w->value("canvas").json());
        QCOMPARE(handle->revision, after);
        // The new revision's parent is the one the region read.
        QCOMPARE(CanvasStore::forSession().revision(id, after)->parent, before);
        auto doc = CanvasStore::forSession().load(id, after);
        QCOMPARE(doc->layerCount(), 2);
        QCOMPARE(doc->layer(1).name, QString("Green"));
        // Green inside the rectangle (60..160, 50..130), the old picture in the padding and outside.
        QCOMPARE(pixel(id, after, 100, 80, 1), 255);
        QCOMPARE(pixel(id, after, 100, 80, 0), 0);
        QCOMPARE(pixel(id, after, 52, 42, 0), 200); // padding, inside the red block: untouched
        QCOMPARE(pixel(id, after, 300, 200, 2), 220);
        // The old revision is as it was.
        QCOMPARE(pixel(id, before, 100, 80, 0), 200);
    }

    // #229: a run lands on a head that moved meanwhile, as a new revision with the layer on top of the other change.
    void aWriteLosingTheRaceLandsItsLayerOnTheMovedHead() {
        Bench bench;
        const QString id = makeCanvas();
        const QString base = headOf(id);
        const Result<Values> r = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}});
        // Somebody else moves the head while the workflow runs.
        auto doc = CanvasStore::forSession().load(id, base);
        doc->loadLayer(LayerKind::Paint, "Theirs", true, 1, mask::MaskPaint(kW, kH, std::vector<uint8_t>(size_t(kW) * kH * 4, 128), 4));
        const auto theirs = CanvasStore::forSession().commit(id, doc.value(), base, "someone");
        QVERIFY(theirs && theirs->move.moved);

        const Result<Values> w = writeGreen(bench, r, {{"name", Value("Green")}});
        QVERIFY2(w.ok(), qPrintable(codeOf(w)));
        QVERIFY(w.problems().isEmpty());
        QVERIFY(w->value("accepted").json().toBool());
        const QString now = headOf(id);
        QVERIFY(now != theirs->revision.id && now != base);
        QCOMPARE(w->value("head").text(), now);
        const auto handle = canvas::canvasHandleFrom(w->value("canvas").json());
        QCOMPARE(handle->revision, now);
        // Their change is kept, and the run's layer sits on top of it.
        QCOMPARE(CanvasStore::forSession().revision(id, now)->parent, theirs->revision.id);
        auto landed = CanvasStore::forSession().load(id, now);
        QCOMPARE(landed->layerCount(), 3);
        QCOMPARE(landed->layer(1).name, QString("Theirs"));
        QCOMPARE(landed->layer(2).name, QString("Green"));
        QCOMPARE(pixel(id, now, 100, 80, 1), 255);
        QVERIFY(CanvasStore::forSession().candidates(id).value().empty());
        // Undo takes the run's layer away and leaves theirs.
        QVERIFY(CanvasStore::forSession().undo(id, now)->moved);
        QCOMPARE(headOf(id), theirs->revision.id);
        QCOMPARE(CanvasStore::forSession().load(id, headOf(id))->layerCount(), 2);
    }

    // #239: a head move the run cannot be placed on (the canvas has another size now) fails the run in plain words and writes nothing.
    void aWriteThatCannotBePlacedSaysSoAndWritesNothing() {
        Bench bench;
        const QString id = makeCanvas();
        const QString base = headOf(id);
        const Result<Values> r = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}});
        mask::PaintDocument smaller(kW / 2, kH / 2); // the canvas has another size now
        smaller.loadLayer(LayerKind::Paint, "Background", true, 1, mask::MaskPaint(kW / 2, kH / 2, std::vector<uint8_t>(size_t(kW / 2) * (kH / 2) * 4, 255), 4));
        const auto theirs = CanvasStore::forSession().commit(id, smaller, base, "someone");
        QVERIFY(theirs && theirs->move.moved);

        const Result<Values> w = writeGreen(bench, r);
        QVERIFY(!w.ok());
        QCOMPARE(codeOf(w), QString("canvas_head_moved"));
        QVERIFY(w.errors().first().message.contains("nothing was written"));
        QCOMPARE(headOf(id), theirs->revision.id);
        // No hidden revision: the history is the base and theirs, and nothing waits for review.
        QCOMPARE(CanvasStore::forSession().log(id)->size(), size_t(2));
        QVERIFY(CanvasStore::forSession().candidates(id, true).value().empty());
    }

    // #274: a write on a canvas with a group keeps the group and puts the layer on top, and so does a resize.
    void aWriteOnACanvasWithAGroupKeepsTheGroup() {
        Bench bench;
        const QString id = makeCanvas();
        canvas::LayerEdit group;
        group.op = canvas::LayerOp::Group;
        group.layers = {0};
        const auto grouped = CanvasStore::forSession().editLayers(id, headOf(id), group, "canvas-view:group");
        QVERIFY(grouped && grouped->move.moved);
        const Result<Values> r = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}});
        const Result<Values> w = writeGreen(bench, r);
        QVERIFY2(w.ok(), qPrintable(w.errors().value(0).message));
        const canvas::Tree written = CanvasStore::forSession().loadTree(id, headOf(id)).value();
        QCOMPARE(written.layers.size(), size_t(2));
        QVERIFY(written.layers[0].group);
        QVERIFY(!written.layers[1].group);
        QCOMPARE(CanvasStore::forSession().revision(id, headOf(id))->parent, grouped->revision.id);
        const auto resized = CanvasStore::forSession().resize(id, headOf(id), mask::IRect{0, 0, 10, 10}, edgefill::Mode::Edge, 0, "x");
        QVERIFY2(resized && resized->move.moved, resized ? "" : qPrintable(resized.errors().value(0).message));
        const canvas::Tree small = CanvasStore::forSession().loadTree(id, headOf(id)).value();
        QVERIFY(small.layers[0].group);
        QCOMPARE(small.width, 10);
    }

    // #274: the next "Fill n" counts the layers inside groups too.
    void theNextFillNameCountsLayersInsideGroups() {
        Bench bench;
        const QString id = makeCanvas();
        canvas::LayerEdit rename;
        rename.op = canvas::LayerOp::Rename;
        rename.layers = {0};
        rename.text = "Fill 7";
        QVERIFY(CanvasStore::forSession().editLayers(id, headOf(id), rename, "x")->move.moved);
        canvas::LayerEdit group;
        group.op = canvas::LayerOp::Group;
        group.layers = {0};
        QVERIFY(CanvasStore::forSession().editLayers(id, headOf(id), group, "x")->move.moved);
        const Result<Values> r = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}});
        const Result<Values> w = writeGreen(bench, r);
        QVERIFY2(w.ok(), qPrintable(w.errors().value(0).message));
        QCOMPARE(CanvasStore::forSession().loadTree(id, headOf(id))->layers.back().name, QString("Fill 8"));
    }

    // #239: candidate review is not a user concept: an old workflow's saved candidate=true is ignored, and the layer lands on the head.
    void aSavedCandidateOnIsIgnoredAndTheLayerLandsOnTheHead() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        const NodeType &type = *registry.nodeType("fugue/canvas-write");
        QVERIFY(!type.input("candidate"));
        QVERIFY(type.retiredInputs.contains("candidate"));

        QTemporaryDir dir;
        Kernels kernels;
        nodes::addBuiltinKernels(kernels);
        FileHashes hashes(dir.filePath("hashes.json"));
        ResultCache cache(dir.filePath("results"));
        const QString id = makeCanvas();
        const QString before = headOf(id);
        // A workflow saved with Write 1.3.0 and Keep as candidate on.
        const QByteArray text = QByteArray("{\"fugue\": 1, \"graph\": {\"nodes\": {"
                                           "\"r\": {\"type\": \"fugue/canvas-region\", \"version\": \"1.0.0\", \"inputs\": {\"canvas\": \"") + id.toUtf8() +
                                "\", \"x\": 60, \"y\": 50, \"width\": 100, \"height\": 80, \"padding\": 16}},"
                                "\"w\": {\"type\": \"fugue/canvas-write\", \"version\": \"1.3.0\", \"inputs\": {"
                                "\"canvas\": {\"from\": \"r.canvas\"}, \"image\": {\"from\": \"r.image\"}, \"stitch\": {\"from\": \"r.stitch\"}, \"blend\": 4, \"candidate\": true}}}}}";
        const Document document = Document::load(text).value();
        RunCompileSetup setup;
        setup.resolveCanvas = sessionCanvasResolver();
        const Compilation compiled = compileForRun(document, registry, {}, setup);
        QVERIFY2(compiled.ok(), qPrintable(compiled.errors().value(0).message));
        QVERIFY2(compiled.warnings().isEmpty(), qPrintable(compiled.warnings().value(0).message)); // no notice either
        const auto folder = RunFolder::create(dir.filePath("runs"));
        RunEnvironment environment;
        environment.registry = &registry;
        environment.kernels = &kernels;
        environment.cache = &cache;
        environment.hashes = &hashes;
        const RunOutcome outcome = runGraph(*compiled, *folder, environment, {});
        QVERIFY2(outcome.status == RunStatus::Ok, qPrintable(outcome.errors.value(0).message));
        QVERIFY(headOf(id) != before);
        QVERIFY(outcome.outputs.value("w").value("accepted").json().toBool());
        QVERIFY(CanvasStore::forSession().candidates(id).value().empty());
    }

    void withoutAStitchTheImageCoversTheWholeCanvas() {
        Bench bench;
        const QString id = makeCanvas();
        const Result<Values> r = bench.region(id);
        Result<Values> red = imageOutput(solid(200, 150, 1, 0, 0)); // half size: resized to the canvas
        const Result<Values> w = bench.run("fugue/canvas-write", {{"canvas", r->value("canvas")}, {"image", red->value("image")}, {"blend", Value(0)}}, "whole");
        QVERIFY2(w.ok(), qPrintable(codeOf(w)));
        const QString head = headOf(id);
        QCOMPARE(pixel(id, head, 0, 0, 0), 255);
        QCOMPARE(pixel(id, head, 399, 299, 3), 255);
        QCOMPARE(pixel(id, head, 300, 200, 2), 0);
    }

    void aMaskOnTheWriteReplacesTheStitchsAndBlendSoftensIt() {
        Bench bench;
        const QString id = makeCanvas();
        const Result<Values> r = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}, {"padding", Value(30)}});
        Pixels m(kW, kH, 1);
        for (int y = 80; y < 100; ++y)
            for (int x = 100; x < 120; ++x)
                m.at(x, y, 0) = 1;
        Result<Values> maskValue = namedOutput(m, "mask");
        const MediaHandle file = maskValue->value("mask").media().saved(bench.dir.path(), "wm", bench.hashes).value();
        const Result<Values> w = writeGreen(bench, r, {{"mask", Value(file)}, {"blend", Value(6)}});
        QVERIFY2(w.ok(), qPrintable(codeOf(w)));
        const QString head = headOf(id);
        QCOMPARE(pixel(id, head, 110, 90, 1), 255); // green where the mask is
        const int edge = pixel(id, head, 98, 90, 1);
        QVERIFY2(edge > 30 && edge < 255, qPrintable(QString::number(edge))); // soft, outward of the mask
        QCOMPARE(pixel(id, head, 70, 60, 1), 30);   // far from the mask the red block shows: the stitch's own mask is not used
        QCOMPARE(pixel(id, head, 70, 60, 0), 200);
    }


    // #226: the run's layer arrives with its blend as a layer mask; Bake gives the old layer, the same picture.
    void aWritesLayerHasTheBlendAsItsMaskAndBakeIsTheSamePicture() {
        Bench bench;
        const QString id = makeCanvas();
        const QString base = headOf(id);
        const Result<Values> r = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}, {"padding", Value(30)}});
        const Result<Values> masked = writeGreen(bench, r, {{"blend", Value(12)}}, "masked");
        QVERIFY2(masked.ok(), qPrintable(codeOf(masked)));
        const QString a = canvas::canvasHandleFrom(masked->value("canvas").json())->revision;
        // The same fill on a second canvas, baked.
        const QString id2 = makeCanvas();
        const Result<Values> r2 = bench.region(id2, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}, {"padding", Value(30)}});
        const Result<Values> baked = writeGreen(bench, r2, {{"blend", Value(12)}, {"bake", Value(true)}}, "baked");
        QVERIFY2(baked.ok(), qPrintable(codeOf(baked)));
        const QString b = canvas::canvasHandleFrom(baked->value("canvas").json())->revision;
        const PaintDocument withMask = CanvasStore::forSession().load(id, a).value();
        const PaintDocument withBake = CanvasStore::forSession().load(id2, b).value();
        const mask::LayerInfo top = withMask.layer(1);
        QVERIFY(top.hasMask && top.maskEnabled);
        QVERIFY(!withBake.layer(1).hasMask);
        // The layer holds the whole redrawn picture (padding included); the mask hides the padding.
        QCOMPARE(int(withMask.pixels(top.id)->rgbaAt(35, 25).a), 255);
        QCOMPARE(int(withMask.maskPixels(top.id)->at(35, 25)), 0);
        const std::vector<uint8_t> p = withMask.flatten().pixels(), q = withBake.flatten().pixels();
        int worst = 0;
        for (size_t i = 0; i < p.size(); ++i)
            worst = std::max(worst, std::abs(int(p[i]) - int(q[i])));
        QVERIFY2(worst <= 1, qPrintable(QString::number(worst)));
        // With its mask off, the layer shows whole: the redrawn picture over the padding too.
        PaintDocument off = withMask.scaledTo(kW, kH);
        off.setMaskEnabled(off.layer(1).id, false);
        QCOMPARE(int(off.flatten().rgbaAt(35, 25).g), 255);
        QCOMPARE(CanvasStore::forSession().revision(id, a)->parent, base);
    }

    // #226: Fill (the default) names the layer from the prompt upstream, else Fill 1, Fill 2; its origin is kept.
    void aWritesLayerIsNamedFromThePromptOrNumberedAndKnowsWhereItCameFrom() {
        Bench bench;
        const QString id = makeCanvas();
        const Result<Values> r = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}});
        QVERIFY(writeGreen(bench, r, {}, "first").ok());
        QCOMPARE(CanvasStore::forSession().load(id, headOf(id))->layer(1).name, QString("Fill 1"));
        const Result<Values> again = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}}, "region2");
        QVERIFY(writeGreen(bench, again, {}, "second").ok());
        const PaintDocument two = CanvasStore::forSession().load(id, headOf(id)).value();
        QCOMPARE(two.layer(2).name, QString("Fill 2"));
        QCOMPARE(two.layer(2).producer, QString("canvas-write:second"));
        QVERIFY(QDateTime::fromString(two.layer(2).created, Qt::ISODateWithMs).isValid());

        // The workflow as run: write <- decode <- sampler <- encode (prompt).
        const QJsonObject document{{"graph", QJsonObject{{"nodes", QJsonObject{
            {"encode", QJsonObject{{"type", "fugue/text-encode"}, {"inputs", QJsonObject{{"prompt", "  A potted   fern on the floor, in the same warm light of the evening  "}}}}},
            {"sampler", QJsonObject{{"type", "x"}, {"inputs", QJsonObject{{"conditioning", QJsonObject{{"from", "encode.conditioning"}}}}}}},
            {"decode", QJsonObject{{"type", "x"}, {"inputs", QJsonObject{{"samples", QJsonArray{QJsonObject{{"from", "sampler.latent"}}}}}}}},
            {"third", QJsonObject{{"type", "fugue/canvas-write"}, {"inputs", QJsonObject{{"image", QJsonObject{{"from", "decode.image"}}}}}}}}}}}};
        const Result<Values> third = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}}, "region3");
        Result<Values> green = imageOutput(solid(third->value("width").number(), third->value("height").number(), 0, 1, 0));
        const Result<Values> w = bench.run("fugue/canvas-write", {{"canvas", third->value("canvas")}, {"image", green->value("image")}, {"stitch", third->value("stitch")}},
                                           "third", document);
        QVERIFY2(w.ok(), qPrintable(codeOf(w)));
        QCOMPARE(CanvasStore::forSession().load(id, headOf(id))->layer(3).name, QString("A potted fern on the floor, in the same…"));
        // A name given is kept as it is.
        const Result<Values> fourth = bench.region(id, {}, "region4");
        QVERIFY(writeGreen(bench, fourth, {{"name", Value("Sky")}}, "fourth").ok());
        QCOMPARE(CanvasStore::forSession().load(id, headOf(id))->layer(4).name, QString("Sky"));
    }

    // #226: Region reads the composite (visible layers) by default, or one layer by name.
    void aRegionReadsTheCompositeOrOneNamedLayer() {
        Bench bench;
        const QString id = makeCanvas();
        auto doc = CanvasStore::forSession().load(id, headOf(id)).value();
        mask::MaskPaint green(kW, kH, 4);
        green.fill(Rgba{0, 255, 0, 255});
        doc.loadLayer(LayerKind::Paint, "Hidden green", false, 1, green);
        QVERIFY(CanvasStore::forSession().commit(id, doc, headOf(id), "test")->move.moved);
        const Values box{{"x", Value(60)}, {"y", Value(50)}, {"width", Value(16)}, {"height", Value(16)}, {"padding", Value(16)}, {"multiple", Value(1)}};
        const Result<Values> composite = bench.region(id, box, "composite");
        QVERIFY2(composite.ok(), qPrintable(codeOf(composite)));
        QCOMPARE(pixelsOf(composite->value("image"), "image")->at(20, 20, 0), 200.0f / 255.0f); // the red block: the hidden layer is not read
        Values named = box;
        named.insert("layer", Value("Hidden green"));
        const Result<Values> layer = bench.region(id, named, "named");
        QVERIFY2(layer.ok(), qPrintable(codeOf(layer)));
        QCOMPARE(pixelsOf(layer->value("image"), "image")->at(20, 20, 1), 1.0f);
        QCOMPARE(pixelsOf(layer->value("image"), "image")->at(20, 20, 0), 0.0f);
        named.insert("layer", Value("Nope"));
        const Result<Values> missing = bench.region(id, named, "missing");
        QCOMPARE(codeOf(missing), QString("input_invalid"));
        QVERIFY(missing.errors().first().hint.contains("Hidden green"));
    }

    // ---- Write onto a layer mask (1.6.0, #231 slice 6) ----

    // The whole canvas as the region, and a subject mask the size of the region: white in a square.
    Result<Values> subjectOnto(Bench &bench, const QString &id, const QString &layer, Values extra = {}, const QString &work = "onto") {
        const Result<Values> r = bench.region(id, {{"layer", Value(layer)}, {"padding", Value(0)}, {"max_megapixels", Value(0)}, {"multiple", Value(1)}});
        Pixels m(kW, kH, 1);
        for (int y = 40; y < 130; ++y)
            for (int x = 50; x < 170; ++x)
                m.at(x, y, 0) = 1;
        Result<Values> maskValue = namedOutput(m, "mask");
        const MediaHandle file = maskValue->value("mask").media().saved(bench.dir.path(), work + "-mask", bench.hashes).value();
        Values inputs{{"canvas", r->value("canvas")}, {"stitch", r->value("stitch")}, {"layer_mask", Value(file)}, {"onto", Value(layer)}};
        for (auto it = extra.begin(); it != extra.end(); ++it)
            inputs.insert(it.key(), it.value());
        return bench.run("fugue/canvas-write", inputs, work);
    }

    // The layer mask the revision holds on the layer at `position`, as bytes.
    int maskByte(const QString &id, const QString &revision, int position, int x, int y) {
        const auto tree = CanvasStore::forSession().loadTree(id, revision);
        const auto &layer = tree->layers[size_t(position)];
        const auto held = canvas::assembleLayer(kW, kH, 1, layer.maskTiles, [&](const canvas::TileKey &key, const QString &hash) {
            const mask::IRect rect = canvas::tileRect(key, kW, kH);
            return CanvasStore::forSession().tilePixels(hash, rect.width, rect.height, 1);
        });
        return held->at(x, y);
    }

    void aWriteOntoALayerPutsTheMaskOnTheLayerInOneUndoableRevision() {
        Bench bench;
        const QString id = makeCanvas();
        const QString before = headOf(id);
        const Result<Values> w = subjectOnto(bench, id, "Base");
        QVERIFY2(w.ok(), qPrintable(codeOf(w)));
        const QString after = headOf(id);
        QVERIFY(after != before);
        QCOMPARE(w->value("head").text(), after);
        QCOMPARE(CanvasStore::forSession().revision(id, after)->parent, before); // one revision
        const auto tree = CanvasStore::forSession().loadTree(id, after);
        QCOMPARE(int(tree->layers.size()), 1); // no layer was added
        QVERIFY(tree->layers[0].hasMask);
        QCOMPARE(maskByte(id, after, 0, 100, 80), 255); // the mask is what was given
        QCOMPARE(maskByte(id, after, 0, 300, 200), 0);
        QCOMPARE(pixel(id, after, 300, 200, 3), 0);     // the blue block is hidden by the mask, the red one shows
        QCOMPARE(pixel(id, after, 100, 80, 0), 200);
        QCOMPARE(pixel(id, before, 300, 200, 3), 255);  // the revision before is as it was
        const auto undone = CanvasStore::forSession().undo(id, after); // Undo takes it back, as any layer edit
        QVERIFY(undone && undone->moved);
        QCOMPARE(headOf(id), before);
    }

    // Canvas New (#231 slice 8): a picture becomes a canvas of its own, at its own size; the source is not touched; each run makes one more.
    void canvasNewMakesACanvasOfThePicturesSizeAndNumbersATakenName() {
        Bench bench;
        const QString source = makeCanvas();
        const QString before = headOf(source);
        const auto make = [&](const QString &name, int w, int h, const QString &work) {
            Result<Values> picture = imageOutput(solid(w, h, 0, 1, 0));
            Values in{{"image", picture.value().value("image")}, {"name", Value(name)}};
            return bench.run("fugue/canvas-new", in, work);
        };
        // A name of its own: the test home persists between runs, so a fixed name would meet its own earlier canvases.
        const QString base = QStringLiteral("Upscaled ") + QUuid::createUuid().toString(QUuid::Id128).left(8);
        const Result<Values> first = make(base, 800, 600, "new1");
        QVERIFY2(first.ok(), qPrintable(codeOf(first)));
        const QString id = first->value("id").text();
        QVERIFY(id != source && canvas::isCanvasId(id));
        const auto info = CanvasStore::forSession().info(id);
        QCOMPARE(info->name, base);
        const auto tree = CanvasStore::forSession().loadTree(id, info->head);
        QCOMPARE(tree->width, 800);
        QCOMPARE(tree->height, 600);
        QCOMPARE(int(tree->layers.size()), 1); // one layer holding the picture whole, one revision
        QCOMPARE(tree->layers.front().name, base); // named for the canvas's name, not "Layer 1" (#311)
        QCOMPARE(pixel(id, info->head, 799, 599, 1), 255);
        QCOMPARE(pixel(id, info->head, 10, 10, 0), 0);
        QCOMPARE(headOf(source), before); // the source canvas is as it was
        // The same input again is another canvas (the node is never cached), and the taken name gets a number.
        const Result<Values> second = make(base, 800, 600, "new2");
        QVERIFY2(second.ok(), qPrintable(codeOf(second)));
        QVERIFY(second->value("id").text() != id);
        QCOMPARE(CanvasStore::forSession().info(second->value("id").text())->name, base + QStringLiteral(" 2"));
        // The layer takes the name asked for, without the number a taken canvas name got.
        const auto secondTree = CanvasStore::forSession().loadTree(second->value("id").text(), CanvasStore::forSession().info(second->value("id").text())->head);
        QCOMPARE(secondTree->layers.front().name, base);
        QVERIFY(!registryNode(bench, "fugue/canvas-new").deterministic);
    }

    void canvasNewRefusesAPictureNoCanvasCanBeAndMakesNothing() {
        Bench bench;
        const size_t before = CanvasStore::forSession().list().size();
        Result<Values> huge = imageOutput(solid(16385, 8, 0, 1, 0));
        const Result<Values> made = bench.run("fugue/canvas-new", Values{{"image", huge.value().value("image")}}, "toobig");
        QVERIFY(!made.ok());
        QCOMPARE(codeOf(made), QStringLiteral("canvas_too_big"));
        QCOMPARE(CanvasStore::forSession().list().size(), before);
    }

    void aWriteOntoALockedOrMissingLayerSaysSoAndWritesNothing() {
        Bench bench;
        const QString id = makeCanvas();
        const QString before = headOf(id);
        QCOMPARE(codeOf(subjectOnto(bench, id, "Base", {{"onto", Value("Nowhere")}}, "missing")), QString("input_invalid"));
        canvas::LayerEdit lock;
        lock.op = canvas::LayerOp::Lock;
        lock.layers = {0};
        lock.flag = true;
        const auto locked = CanvasStore::forSession().editLayers(id, before, lock, "test");
        QVERIFY(locked && locked->move.moved);
        const QString lockedHead = headOf(id);
        const Result<Values> w = subjectOnto(bench, id, "Base", {}, "locked");
        QCOMPARE(codeOf(w), QString("input_invalid"));
        QVERIFY(w.errors().first().message.contains("locked"));
        QCOMPARE(headOf(id), lockedHead);
    }

    void aWriteOntoNeedsALayerMaskAndKeepsTheOldMaskOutsideTheSelection() {
        Bench bench;
        const QString id = makeCanvas();
        const Result<Values> r = bench.region(id, {{"layer", Value("Base")}});
        QCOMPARE(codeOf(bench.run("fugue/canvas-write", {{"canvas", r->value("canvas")}, {"onto", Value("Base")}}, "none")), QString("input_missing"));
        // First the whole layer: everything hidden but the square. Then only a rectangle of the selection is looked at again:
        // inside it the new mask replaces the old one, outside it the old mask stays.
        QVERIFY(subjectOnto(bench, id, "Base").ok());
        const Result<Values> inner = bench.region(id, {{"layer", Value("Base")}, {"x", Value(200)}, {"y", Value(100)}, {"width", Value(150)}, {"height", Value(150)},
                                                      {"padding", Value(0)}, {"max_megapixels", Value(0)}, {"multiple", Value(1)}}, "inner");
        Pixels all(150, 150, 1, 1.0f); // the subject is all of the 150 x 150 region
        Result<Values> maskValue = namedOutput(all, "mask");
        const MediaHandle file = maskValue->value("mask").media().saved(bench.dir.path(), "inner-mask", bench.hashes).value();
        const Result<Values> w = bench.run("fugue/canvas-write", {{"canvas", inner->value("canvas")}, {"stitch", inner->value("stitch")}, {"layer_mask", Value(file)}, {"onto", Value("Base")}}, "inner-write");
        QVERIFY2(w.ok(), qPrintable(codeOf(w)));
        const QString head = headOf(id);
        QCOMPARE(maskByte(id, head, 0, 300, 200), 255); // inside the selection: now shown
        QCOMPARE(maskByte(id, head, 0, 100, 80), 255);  // outside it: the old square stays
        QCOMPARE(maskByte(id, head, 0, 10, 10), 0);     // outside, hidden before and still
    }

    void aPictureOfAnotherShapeIsRefusedAndNothingIsWritten() {
        Bench bench;
        const QString id = makeCanvas();
        const QString before = headOf(id);
        const Result<Values> r = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}});
        Result<Values> wrong = imageOutput(solid(100, 100, 0, 1, 0));
        const Result<Values> w = bench.run("fugue/canvas-write", {{"canvas", r->value("canvas")}, {"image", wrong->value("image")}, {"stitch", r->value("stitch")}}, "wrong");
        QCOMPARE(codeOf(w), QString("stitch_mismatch"));
        QCOMPARE(headOf(id), before);
        QCOMPARE(codeOf(bench.run("fugue/canvas-write", {{"canvas", Value("nope")}, {"image", wrong->value("image")}}, "nohandle")), QString("input_invalid"));
    }

    // ---- through the scheduler ----

    void aRerunOfTheSameRevisionIsACacheHitAndWritesNothingTwice() {
        QTemporaryDir dir;
        Registry registry;
        Kernels kernels;
        nodes::addBuiltinNodeTypes(registry);
        nodes::addBuiltinKernels(kernels);
        FileHashes hashes(dir.filePath("hashes.json"));
        ResultCache cache(dir.filePath("results"));

        const QString id = makeCanvas();
        const QByteArray text = QByteArray("{\"fugue\": 1, \"graph\": {\"nodes\": {"
                                           "\"r\": {\"type\": \"fugue/canvas-region\", \"version\": \"1.0.0\", \"inputs\": {\"canvas\": \"") + id.toUtf8() +
                                "\", \"x\": 60, \"y\": 50, \"width\": 100, \"height\": 80, \"padding\": 16}},"
                                "\"w\": {\"type\": \"fugue/canvas-write\", \"version\": \"1.0.0\", \"inputs\": {"
                                "\"canvas\": {\"from\": \"r.canvas\"}, \"image\": {\"from\": \"r.image\"}, \"stitch\": {\"from\": \"r.stitch\"}, \"blend\": 4}}}}}";
        const Document document = Document::load(text).value();
        RunCompileSetup setup;
        setup.resolveCanvas = sessionCanvasResolver();

        const QString base = headOf(id);
        Registry compileRegistry;
        nodes::addBuiltinNodeTypes(compileRegistry);
        const Compilation compiled = compileForRun(document, compileRegistry, {}, setup);
        QVERIFY2(compiled.ok(), qPrintable(compiled.errors().value(0).message));
        // Head is named once, in the graph: the revision the canvas is at now.
        QCOMPARE(compiled->nodes.value("r").inputs.value("revision").literal().toString(), base);

        const auto runOnce = [&]() {
            const auto folder = RunFolder::create(dir.filePath("runs"));
            RunEnvironment environment;
            environment.registry = &registry;
            environment.kernels = &kernels;
            environment.cache = &cache;
            environment.hashes = &hashes;
            return runGraph(*compiled, *folder, environment, {});
        };
        const RunOutcome first = runOnce();
        QVERIFY2(first.status == RunStatus::Ok, qPrintable(first.errors.value(0).message));
        QCOMPARE(first.ran.size(), 2);
        const QString moved = headOf(id);
        QVERIFY(moved != base);
        QVERIFY(first.outputs.value("w").value("accepted").json().toBool());

        // The same compiled graph again: both nodes hit, nothing is written, the head stays.
        const RunOutcome second = runOnce();
        QVERIFY(second.status == RunStatus::Ok);
        QVERIFY2(second.ran.isEmpty(), qPrintable(second.ran.join(',')));
        QCOMPARE(second.states.value("r"), NodeState::Cached);
        QCOMPARE(second.keys.value("r"), first.keys.value("r"));
        QCOMPARE(headOf(id), moved);
        QCOMPARE(CanvasStore::forSession().log(id)->size(), size_t(2));

        // The head moves elsewhere (#229): the same compiled graph is still a cache hit and adds no second layer.
        {
            auto other = CanvasStore::forSession().load(id, moved);
            other->loadLayer(LayerKind::Paint, "Elsewhere", true, 1, mask::MaskPaint(kW, kH, std::vector<uint8_t>(size_t(kW) * kH * 4, 128), 4));
            QVERIFY(CanvasStore::forSession().commit(id, other.value(), moved, "someone")->move.moved);
            const RunOutcome cached = runOnce();
            QVERIFY(cached.ran.isEmpty());
            QCOMPARE(CanvasStore::forSession().log(id)->size(), size_t(3));
        }
        const QString elsewhere = headOf(id);

        // A new compile names the head as it has become: a different key, so Region runs again.
        Registry again;
        nodes::addBuiltinNodeTypes(again);
        const Compilation recompiled = compileForRun(document, again, {}, setup);
        QCOMPARE(recompiled->nodes.value("r").inputs.value("revision").literal().toString(), elsewhere);
        const auto folder = RunFolder::create(dir.filePath("runs"));
        RunEnvironment environment;
        environment.registry = &registry;
        environment.kernels = &kernels;
        environment.cache = &cache;
        environment.hashes = &hashes;
        const RunOutcome third = runGraph(*recompiled, *folder, environment, {});
        QVERIFY(third.ran.contains("r"));
        QVERIFY(third.keys.value("r") != first.keys.value("r"));
        QCOMPARE(CanvasStore::forSession().log(id)->size(), size_t(4));
    }
};

QTEST_GUILESS_MAIN(TestCanvasNodes)
#include "test_canvas_nodes.moc"
