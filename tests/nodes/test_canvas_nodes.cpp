// Canvas Region and Canvas Write (#209, epic #206, spec 14.5).
//
// Seams: the geometry (nodes/canvas_region_ops.h) on float buffers; each node's kernel run as the
// scheduler runs it, against the session's canvas store (the test's own home); and a workflow run
// through compileForRun and the real scheduler for what only the engine can show: that the revision
// is named once, that the same revision gives the same bytes, that a re-run of the compiled graph is a
// cache hit which writes nothing twice, and that a Write moves the head by compare-and-swap.

#include <QDir>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <vector>

#include "engine/canvas_handle.h"
#include "engine/canvas_store.h"
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
    Result<Values> run(const QString &id, const Values &inputs, const QString &work) {
        return kernels.node(id)->run(context(id, inputs, work));
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

    // ---- Canvas Region ----

    void aRegionCutsTheRectangleOfTheRevision() {
        Bench bench;
        const QString id = makeCanvas();
        const Result<Values> r = bench.region(id, {{"x", Value(40)}, {"y", Value(30)}, {"width", Value(150)}, {"height", Value(120)},
                                                   {"padding", Value(0)}, {"scale", Value(1)}, {"max_megapixels", Value(0)}, {"multiple", Value(1)}});
        QVERIFY2(r.ok(), qPrintable(codeOf(r)));
        QCOMPARE(r->value("width").number(), 150.0);
        QCOMPARE(r->value("height").number(), 120.0);
        const Pixels image = pixelsOf(r->value("image"), "image").value();
        QCOMPARE(image.width, 150);
        QCOMPARE(image.height, 120);
        QCOMPARE(image.channels, 4);
        // (50,40) is the red block's corner: at (10,10) in the region. (0,0) is empty canvas.
        QCOMPARE(image.at(10, 10, 0), 200 / 255.0f);
        QCOMPARE(image.at(10, 10, 3), 1.0f);
        QCOMPARE(image.at(0, 0, 3), 0.0f);
        const Pixels mask = maskOf(r->value("mask"), "mask").value();
        QCOMPARE(mask.at(75, 60, 0), 1.0f); // the rectangle is all of it with no padding
        // The handle says which canvas and revision was read.
        const auto handle = canvas::canvasHandleFrom(r->value("canvas").json());
        QVERIFY(handle);
        QCOMPARE(handle->canvas, id);
        QCOMPARE(handle->revision, headOf(id));
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
        const Result<Values> r = bench.region(id, {{"mask", Value(file)}, {"padding", Value(10)}, {"max_megapixels", Value(0)}, {"multiple", Value(1)},
                                                   {"x", Value(0)}, {"width", Value(5)}});
        QVERIFY2(r.ok(), qPrintable(codeOf(r)));
        QCOMPARE(r->value("width").number(), 60.0); // 40 + 2 x 10: the rectangle inputs are not used
        QCOMPARE(r->value("height").number(), 60.0);
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
        const Values rect{{"x", Value(0)}, {"y", Value(0)}, {"width", Value(200)}, {"height", Value(100)}, {"padding", Value(0)}};
        Values doubled = rect;
        doubled.insert("scale", Value(2));
        doubled.insert("max_megapixels", Value(0));
        const Result<Values> big = bench.region(id, doubled);
        QCOMPARE(big->value("width").number(), 400.0);
        QCOMPARE(big->value("height").number(), 208.0); // 200 snapped to the nearest multiple of 16
        Values capped = rect;
        capped.insert("scale", Value(4));
        capped.insert("max_megapixels", Value(0.05));
        const Result<Values> small = bench.region(id, capped);
        QVERIFY(small->value("width").number() * small->value("height").number() <= 50000.0);
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

    void aWriteLosingTheRaceKeepsACandidateAndSaysSo() {
        Bench bench;
        const QString id = makeCanvas();
        const QString base = headOf(id);
        const Result<Values> r = bench.region(id, {{"x", Value(60)}, {"y", Value(50)}, {"width", Value(100)}, {"height", Value(80)}});
        // Somebody else moves the head while the workflow runs.
        auto doc = CanvasStore::forSession().load(id, base);
        doc->loadLayer(LayerKind::Paint, "Theirs", true, 1, mask::MaskPaint(kW, kH, std::vector<uint8_t>(size_t(kW) * kH * 4, 128), 4));
        const auto theirs = CanvasStore::forSession().commit(id, doc.value(), base, "someone");
        QVERIFY(theirs && theirs->move.moved);

        const Result<Values> w = writeGreen(bench, r);
        QVERIFY2(w.ok(), qPrintable(codeOf(w))); // a result, not an error
        QVERIFY(!w->value("accepted").json().toBool());
        QCOMPARE(headOf(id), theirs->revision.id);
        QCOMPARE(w->value("head").text(), theirs->revision.id);
        QCOMPARE(w.problems().first().code, QString("canvas_head_moved"));
        QCOMPARE(w.problems().first().severity, Error::Severity::Warning);
        // The candidate is stored and reachable: its handle names it, and it holds the green layer.
        const auto candidate = canvas::canvasHandleFrom(w->value("canvas").json());
        QVERIFY(candidate && candidate->revision != theirs->revision.id);
        QCOMPARE(CanvasStore::forSession().revision(id, candidate->revision)->parent, base);
        QCOMPARE(pixel(id, candidate->revision, 100, 80, 1), 255);
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

        // A new compile names the head as it has become: a different key, so Region runs again.
        Registry again;
        nodes::addBuiltinNodeTypes(again);
        const Compilation recompiled = compileForRun(document, again, {}, setup);
        QCOMPARE(recompiled->nodes.value("r").inputs.value("revision").literal().toString(), moved);
        const auto folder = RunFolder::create(dir.filePath("runs"));
        RunEnvironment environment;
        environment.registry = &registry;
        environment.kernels = &kernels;
        environment.cache = &cache;
        environment.hashes = &hashes;
        const RunOutcome third = runGraph(*recompiled, *folder, environment, {});
        QVERIFY(third.ran.contains("r"));
        QVERIFY(third.keys.value("r") != first.keys.value("r"));
        QCOMPARE(CanvasStore::forSession().log(id)->size(), size_t(3));
    }
};

QTEST_GUILESS_MAIN(TestCanvasNodes)
#include "test_canvas_nodes.moc"
