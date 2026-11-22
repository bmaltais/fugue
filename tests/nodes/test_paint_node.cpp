// The Paint node (#181, spec 14.4): the layers a `paint` input names put over a picture, or over a
// blank canvas, as an image and a mask; and a `paint` input as a file that travels with the
// workflow, as a `mask` input does (the scheduler's relative path, read roots and content-hash key).
//
// Seam: the registered kernel, called as the scheduler calls it (NodeKernel::run with a
// NodeContext), and the scheduler itself through the engine tests' World. Pictures are written with
// libvips and canvases with PaintDocument, in the test's own temporary folders.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <vector>

#include "../engine/scheduler_support.h"
#include "engine/file_hashes.h"
#include "engine/paint_document.h"
#include "engine/paint_file.h"
#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"
#include "nodes/paint_input.h"
#include "nodes/pixel_io.h"

using namespace fugue;
using namespace fugue::mask;

namespace {

QString writePng(const QDir &dir, const QString &name, int w, int h, int bands, int value) {
    std::vector<unsigned char> bytes(size_t(w) * h * bands, static_cast<unsigned char>(value));
    initVips();
    vips::VImage image = vips::VImage::new_from_memory_copy(bytes.data(), bytes.size(), w, h, bands, VIPS_FORMAT_UCHAR);
    const QString path = dir.filePath(name);
    image.write_to_file(path.toUtf8().constData());
    return path;
}

// A canvas with a picture placeholder, a red square on a paint layer and a white square on a mask layer.
PaintDocument canvas(int w, int h) {
    PaintDocument d(w, h);
    d.addLayer(LayerKind::Picture, "Picture");
    d.addLayer(LayerKind::Paint, "Red");
    d.brush.color = Rgba{255, 0, 0, 255};
    d.fillRect(0, 0, w / 2.0, h / 2.0);
    d.addLayer(LayerKind::Mask, "Region");
    d.fillRect(w / 2.0, h / 2.0, w / 2.0, h / 2.0);
    return d;
}

float at(const Value &value, int x, int y, int channel) {
    const Result<nodes::Pixels> pixels = nodes::pixelsOf(value, "x");
    return pixels.ok() ? pixels->at(x, y, channel) : -1.0f;
}

} // namespace

class TestPaintNode : public QObject {
    Q_OBJECT

    Kernels m_kernels;
    QTemporaryDir m_dir;
    std::unique_ptr<FileHashes> m_hashes;

    QDir dir() const { return QDir(m_dir.path()); }

    Result<Values> run(const Values &inputs, const std::optional<FileRoots> &roots = std::nullopt) {
        const std::shared_ptr<NodeKernel> kernel = m_kernels.node("fugue/paint");
        if (!kernel)
            return Result<Values>(Errors{makeError("internal", "no kernel fugue/paint")});
        NodeContext context;
        context.node = "n";
        context.inputs = inputs;
        context.workDir = m_dir.path();
        context.hashes = m_hashes.get();
        context.roots = roots;
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        return kernel->run(context);
    }

    QString save(const PaintDocument &d, const QString &name) {
        const QString path = dir().filePath(name);
        [&] { QVERIFY(!hasErrors(writePaintFile(d, path))); }();
        return path;
    }

    static bool hasWarning(const Result<Values> &result, const QString &code) {
        for (const Error &warning : result.warnings())
            if (warning.code == code)
                return true;
        return false;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        nodes::addBuiltinKernels(m_kernels);
        m_hashes = std::make_unique<FileHashes>(dir().filePath("hashes.json"));
    }

    // ---- the manifest ----

    void thePaintWidgetIsAnInputFileLikeTheMaskWidget() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        const NodeType *paint = registry.nodeType("fugue/paint");
        QVERIFY(paint);
        QCOMPARE(paint->input("paint")->widget(), QStringLiteral("paint"));
        QCOMPARE(paint->input("paint")->over, QStringLiteral("image"));
        QVERIFY(paint->input("paint")->holdsInputFile());
        QVERIFY(paint->input("paint")->optional);
        QVERIFY(paint->input("image")->optional);
        QCOMPARE(paint->output("mask")->role, QString(kMaskRole));
        QVERIFY(paint->output("image"));
        QVERIFY(!paint->input("width")->holdsInputFile());
    }

    // ---- the kernel ----

    void aBlankCanvasIsTransparentWithAnEmptyMask() {
        const Result<Values> r = run({{"width", Value(12)}, {"height", Value(7)}});
        QVERIFY2(r.ok(), qPrintable(r.error().message));
        QCOMPARE(r.value().value("image").media().width(), 12);
        QCOMPARE(r.value().value("image").media().height(), 7);
        QCOMPARE(at(r.value().value("image"), 3, 3, 3), 0.0f); // alpha
        QCOMPARE(r.value().value("mask").media().width(), 12);
        QCOMPARE(at(r.value().value("mask"), 3, 3, 0), 0.0f);
    }

    void thePaintOnABlankCanvasAndItsMaskComeOut() {
        const QString file = save(canvas(16, 16), "blank.ora");
        const Result<Values> r = run({{"width", Value(16)}, {"height", Value(16)}, {"paint", Value(file)}});
        QVERIFY2(r.ok(), qPrintable(r.error().message));
        QVERIFY(r.warnings().isEmpty());
        const Value image = r.value().value("image"), mask = r.value().value("mask");
        QCOMPARE(at(image, 2, 2, 0), 1.0f);  // red
        QCOMPARE(at(image, 2, 2, 3), 1.0f);
        QCOMPARE(at(image, 12, 12, 3), 0.0f); // nothing there
        QCOMPARE(at(mask, 12, 12, 0), 1.0f);
        QCOMPARE(at(mask, 2, 2, 0), 0.0f);
    }

    void theSizeOfTheFileIsTheCanvasWhenNothingElseSaysSo() {
        const QString file = save(canvas(10, 6), "own-size.ora");
        const Result<Values> r = run({{"paint", Value(file)}});
        QVERIFY2(r.ok(), qPrintable(r.error().message));
        QCOMPARE(r.value().value("image").media().width(), 10);
        QCOMPARE(r.value().value("image").media().height(), 6);
        QVERIFY(r.warnings().isEmpty());
    }

    void thePictureIsUnderThePaintAndUnpaintedGivesThePictureItself() {
        const QString picture = writePng(dir(), "photo.png", 16, 16, 3, 90);
        const Value image = Value(MediaHandle::probe(MediaType::Image, picture, *m_hashes).value());
        const QString file = save(canvas(16, 16), "over.ora");
        const Result<Values> painted = run({{"image", image}, {"paint", Value(file)}});
        QVERIFY2(painted.ok(), qPrintable(painted.error().message));
        QVERIFY(painted.warnings().isEmpty());
        QCOMPARE(at(painted.value().value("image"), 2, 2, 0), 1.0f);                 // red over the picture
        QCOMPARE(at(painted.value().value("image"), 12, 12, 0), 90.0f / 255.0f);     // the picture
        QCOMPARE(at(painted.value().value("image"), 12, 12, 3), 1.0f);
        // Nothing painted: the very picture, not a copy.
        const Result<Values> none = run({{"image", image}});
        QVERIFY(none.ok());
        QCOMPARE(none.value().value("image").media().hash(), image.media().hash());
        QCOMPARE(at(none.value().value("mask"), 0, 0, 0), 0.0f);
        // A file that holds only the placeholder is the same.
        PaintDocument bare(16, 16);
        bare.addLayer(LayerKind::Picture, "Picture");
        const Result<Values> bareRun = run({{"image", image}, {"paint", Value(save(bare, "bare.ora"))}});
        QCOMPARE(bareRun.value().value("image").media().hash(), image.media().hash());
    }

    void aCanvasPaintedOnAnotherSizeIsScaledToThePictureWithAWarning() {
        const QString picture = writePng(dir(), "bigger.png", 32, 32, 3, 10);
        const Value image = Value(MediaHandle::probe(MediaType::Image, picture, *m_hashes).value());
        const QString file = save(canvas(16, 16), "smaller.ora");
        const Result<Values> r = run({{"image", image}, {"paint", Value(file)}});
        QVERIFY2(r.ok(), qPrintable(r.error().message));
        QVERIFY(hasWarning(r, "mask_resized"));
        QCOMPARE(r.value().value("image").media().width(), 32);
        QCOMPARE(r.value().value("mask").media().width(), 32);
        QCOMPARE(at(r.value().value("image"), 4, 4, 0), 1.0f);
        QCOMPARE(at(r.value().value("mask"), 28, 28, 0), 1.0f);
        // Width and Height that differ from the file do the same on a blank canvas.
        const Result<Values> blank = run({{"width", Value(20)}, {"height", Value(20)}, {"paint", Value(file)}});
        QVERIFY(blank.ok() && hasWarning(blank, "mask_resized"));
        QCOMPARE(blank.value().value("image").media().width(), 20);
    }

    void theSameLayersGiveTheSameOutput() {
        const QString file = save(canvas(16, 16), "again.ora");
        const Values inputs{{"width", Value(16)}, {"height", Value(16)}, {"paint", Value(file)}};
        const Result<Values> a = run(inputs), b = run(inputs);
        QVERIFY(a.ok() && b.ok());
        QCOMPARE(a.value().value("image").media().hash(), b.value().value("image").media().hash());
        QCOMPARE(a.value().value("mask").media().hash(), b.value().value("mask").media().hash());
        // And a copy of the file elsewhere gives it too (the path is not in the result).
        const QString copy = dir().filePath("again-copy.ora");
        QVERIFY(QFile::copy(file, copy));
        const Result<Values> c = run({{"width", Value(16)}, {"height", Value(16)}, {"paint", Value(copy)}});
        QCOMPARE(c.value().value("image").media().hash(), a.value().value("image").media().hash());
    }

    void aHiddenLayerDoesNotShow() {
        PaintDocument d = canvas(8, 8);
        d.setVisible(d.layer(1).id, false);
        d.setVisible(d.layer(2).id, false);
        const Result<Values> r = run({{"width", Value(8)}, {"height", Value(8)}, {"paint", Value(save(d, "hidden.ora"))}});
        QCOMPARE(at(r.value().value("image"), 1, 1, 3), 0.0f);
        QCOMPARE(at(r.value().value("mask"), 6, 6, 0), 0.0f);
    }

    void whatCannotBeDrawnIsAnError() {
        Result<Values> r = run({});
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, QStringLiteral("missing_input"));
        r = run({{"width", Value(8)}});
        QVERIFY(!r.ok());
        QCOMPARE(r.error().input, QStringLiteral("height"));
        r = run({{"width", Value(100000)}, {"height", Value(8)}});
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, QStringLiteral("value_out_of_range"));
        r = run({{"width", Value(8)}, {"height", Value(8)}, {"paint", Value(dir().filePath("never.ora"))}});
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, QStringLiteral("file_not_found"));
        QCOMPARE(r.error().input, QStringLiteral("paint"));
        QFile bad(dir().filePath("bad.ora"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not a paint file");
        bad.close();
        r = run({{"width", Value(8)}, {"height", Value(8)}, {"paint", Value(bad.fileName())}});
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, QStringLiteral("media_unreadable"));
        QCOMPARE(r.error().input, QStringLiteral("paint"));
        // The read roots judge the file the kernel reads.
        const std::optional<FileRoots> nowhere = FileRoots{{dir().filePath("elsewhere")}, {}};
        r = run({{"width", Value(8)}, {"height", Value(8)}, {"paint", Value(save(canvas(8, 8), "roots.ora"))}}, nowhere);
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, QStringLiteral("permission_denied"));
    }

    // ---- the scheduler: relocation of the path, read roots, the cache key ----

    void aPaintFileIsKeyedByItsBytesAndReadUnderTheRoots() {
        fugue::test::World world;
        QFile manifest(":/fugue/nodes/paint/node.json");
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        QVERIFY(!hasErrors(world.registry.addManifest(QJsonDocument::fromJson(manifest.readAll()).object())));
        nodes::addPaintInputKernels(world.kernels);

        QTemporaryDir flows;
        QVERIFY(QDir().mkpath(flows.filePath("inputs")));
        const QString first = flows.filePath("inputs/a.ora");
        QVERIFY(!hasErrors(writePaintFile(canvas(16, 16), first)));
        const auto graph = [&](const QString &path) {
            return world.graph(QJsonObject{{"p", QJsonObject{{"type", "fugue/paint"},
                                                             {"inputs", QJsonObject{{"width", 16}, {"height", 16}, {"paint", path}}}}},
                                          {"s", QJsonObject{{"type", "fugue/save"},
                                                            {"inputs", QJsonObject{{"value", QJsonObject{{"from", "p.image"}}}}}}}});
        };
        RunOptions options;
        options.workflow = QDir(flows.path()).canonicalPath() + "/flow.fugue";

        // A relative path is the workflow folder's.
        const fugue::test::Ran one = world.run(graph("inputs/a.ora"), options);
        QVERIFY2(one.outcome.status == RunStatus::Ok, qPrintable(fugue::test::World::dump(one.outcome.errors)));
        QCOMPARE(one.outcome.ran, (QStringList{"p", "s"}));
        QCOMPARE(one.outcome.outputs.value("p").value("image").media().width(), 16);
        // Run again: from the cache.
        QVERIFY(world.run(graph("inputs/a.ora"), options).outcome.ran.isEmpty());
        // The same bytes under another name: the same key, so a hit (the key names the content).
        QVERIFY(QFile::copy(first, flows.filePath("inputs/b.ora")));
        const fugue::test::Ran copy = world.run(graph("inputs/b.ora"), options);
        QVERIFY(copy.outcome.ran.isEmpty());
        QCOMPARE(copy.outcome.keys.value("p"), one.outcome.keys.value("p"));
        // Painted again: other bytes, another key, the node runs.
        PaintDocument changed = canvas(16, 16);
        changed.brush.color = Rgba{0, 0, 255, 255};
        changed.fillRect(4, 4, 4, 4);
        QVERIFY(!hasErrors(writePaintFile(changed, flows.filePath("inputs/c.ora"))));
        const fugue::test::Ran other = world.run(graph("inputs/c.ora"), options);
        QVERIFY(other.outcome.ran.contains("p"));
        QVERIFY(other.outcome.keys.value("p") != one.outcome.keys.value("p"));
        // The read roots are checked before the file is hashed or read.
        RunOptions roots = options;
        roots.roots = FileRoots{{flows.filePath("nowhere")}, {}};
        const fugue::test::Ran denied = world.run(graph("inputs/a.ora"), roots);
        QCOMPARE(denied.outcome.status, RunStatus::Failed);
        QCOMPARE(denied.outcome.errors.first().code, QStringLiteral("permission_denied"));
        QCOMPARE(denied.outcome.errors.first().input, QStringLiteral("paint"));
    }
};

QTEST_GUILESS_MAIN(TestPaintNode)
#include "test_paint_node.moc"
