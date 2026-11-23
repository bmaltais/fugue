// The canvas batch (#250): a canvas action over a folder of pictures. Seam: engine/canvas_batch.h, with a fake runner in
// place of a run (the run itself is the window's and the command line's, tested with them): the runner adds a layer to
// the canvas its bound workflow names, as Canvas Write does. No window, no model, nothing leaves the machine.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/canvas_batch.h"
#include "engine/canvas_store.h"
#include "engine/paint_document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;
using namespace fugue::canvas;

namespace {

const QByteArray kAction = R"({
  "fugue": 1,
  "action": {"format": 1, "name": "Fill", "glyph": "sparkles", "selection": "required", "region": "region",
             "inputs": [{"role": "prompt", "targets": ["encode.prompt"]}, {"role": "seed", "targets": ["sample.seed"]}]},
  "graph": {"nodes": {
    "region": {"type": "fugue/canvas-region", "version": "1.2.0", "inputs": {"canvas": "", "revision": "head", "x": 0, "y": 0, "width": 64, "height": 64, "padding": 32}},
    "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "old words", "references": [{"from": "region.image"}]}},
    "sample": {"type": "fugue/sampler", "version": "1.2.0", "inputs": {"conditioning": {"from": "encode.conditioning"}, "seed": {"auto": "random"}, "steps": 4}},
    "write": {"type": "fugue/canvas-write", "version": "1.3.0",
              "inputs": {"canvas": {"from": "region.canvas"}, "image": {"from": "region.image"}, "stitch": {"from": "region.stitch"}}}
  }}
})";

QString regionCanvas(const Document &bound) { return bound.graph.nodes.value("region").inputs.value("canvas").literal().toString(); }

} // namespace

class TestCanvasBatch : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    Registry m_registry;
    int m_runs = 0;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    // A solid picture; the file's kind is the extension's (png, jpg).
    void picture(const QString &folder, const QString &name, mask::Rgba color = {30, 120, 140, 255}, int w = 24, int h = 16) {
        QDir().mkpath(path(folder));
        mask::MaskPaint paint(w, h, 4);
        paint.fill(color);
        const Result<QByteArray> bytes = paint.toPicture(QFileInfo(name).suffix());
        QVERIFY(bytes);
        QFile file(path(folder + "/" + name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes.value());
    }

    BatchSpec spec(const QString &in, const QString &out) {
        const QString file = path("fill.fugue");
        {
            QFile f(file);
            if (!f.open(QIODevice::WriteOnly))
                qFatal("cannot write the action");
            f.write(kAction);
        }
        const Result<BatchAction> action = loadBatchAction(file, {}, m_registry);
        if (!action)
            qFatal("action: %s", qPrintable(action.error().message));
        BatchSpec s;
        s.action = action.value();
        s.inFolder = path(in);
        s.outFolder = path(out);
        s.prompt = "a hat";
        return s;
    }

    // What Canvas Write does for a run: one new layer on the head of the canvas the workflow names.
    BatchRunner writes(const QString &failOn = {}) {
        return [this, failOn](const Document &bound, const QString &) {
            ++m_runs;
            CanvasStore store = CanvasStore::forSession();
            const QString canvas = regionCanvas(bound);
            const Result<CanvasInfo> info = store.info(canvas);
            if (!info)
                return BatchRunResult{"failed", "canvas_not_found", "no canvas"};
            if (!failOn.isEmpty() && info->name == failOn)
                return BatchRunResult{"failed", "node_failed", "The model fell over."};
            Result<mask::PaintDocument> doc = store.load(canvas, info->head);
            mask::MaskPaint red(doc->width(), doc->height(), 4);
            red.fill(mask::Rgba{200, 20, 20, 255});
            doc->loadLayer(mask::LayerKind::Paint, "Fill", true, 1.0, red);
            const Result<Commit> done = store.commit(canvas, doc.value(), info->head, "fake");
            return done ? BatchRunResult{"ok", {}, {}} : BatchRunResult{"failed", "x", "commit"};
        };
    }

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(m_registry); }
    void init() {
        m_runs = 0;
        CanvasStore store = CanvasStore::forSession(); // under ctest the store is the test's own, and outlives a run
        for (const CanvasInfo &canvas : store.list())
            store.remove(canvas.id);
    }

    void picturesAreListedByNameAndOthersLeftOut() {
        picture("a", "b.png");
        picture("a", "A.jpg");
        picture("a", "c.webp");
        QFile notes(path("a/notes.txt"));
        QVERIFY(notes.open(QIODevice::WriteOnly));
        const Result<QStringList> files = batchPictures(path("a"));
        QVERIFY(files);
        QCOMPARE(files->size(), 3);
        QVERIFY(files->at(0).endsWith("A.jpg"));
        QVERIFY(files->at(2).endsWith("c.webp"));
        QCOMPARE(batchPictures(path("nowhere")).error().code, QString("file_not_found"));
    }

    void outputNamesNeverMeetAnExistingFileOrAnEarlierOne() {
        picture("o", "x.png");
        QSet<QString> taken;
        const QString first = batchOutputPath(path("o"), "x", "png", taken);
        QVERIFY(first.endsWith("x-2.png"));
        taken.insert(first.toLower());
        QVERIFY(batchOutputPath(path("o"), "x", "png", taken).endsWith("x-3.png"));
        QVERIFY(batchOutputPath(path("o"), "y", "png", taken).endsWith("y.png"));
    }

    void everyPictureIsRunAndExportedWithItsOwnName() {
        picture("in1", "one.png", mask::Rgba{255, 0, 0, 255});
        picture("in1", "two.jpg", mask::Rgba{0, 0, 255, 255});
        picture("in1", "two.png", mask::Rgba{0, 255, 0, 255}); // same base name: the second gets a new name
        QJsonArray events;
        BatchHooks hooks;
        hooks.event = [&](const QJsonObject &e) { events.append(e); };
        const Result<BatchReport> report = runBatch(spec("in1", "out1"), writes(), hooks);
        QVERIFY(report);
        QCOMPARE(report->ok(), 3);
        QCOMPARE(m_runs, 3);
        QStringList names;
        for (const BatchItem &item : report->items) {
            QVERIFY(QFile::exists(item.output));
            names << QFileInfo(item.output).fileName();
            QFile file(item.output);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const Result<mask::MaskPaint> out = mask::MaskPaint::fromPicture(file.readAll());
            QVERIFY(out);
            QCOMPARE(out->width(), 24);
            QCOMPARE(out->rgbaAt(3, 3).r, uint8_t(200)); // the action's layer is in
        }
        names.sort();
        QCOMPARE(names, (QStringList{"one.png", "two-2.png", "two.png"}));
        QCOMPARE(events.first().toObject().value("event").toString(), QString("batch.started"));
        QCOMPARE(events.last().toObject().value("ok").toInt(), 3);
        QCOMPARE(events.size(), 1 + 3 * 2 + 1);
        QVERIFY(CanvasStore::forSession().list().empty()); // no temporary canvas is left
    }

    void aFailureIsReportedAndTheBatchGoesOn() {
        picture("in2", "a.png");
        picture("in2", "b.png");
        QFile bad(path("in2/c.png"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not a picture");
        bad.close();
        picture("in2", "d.png");
        const Result<BatchReport> report = runBatch(spec("in2", "out2"), writes("b"));
        QVERIFY(report);
        QCOMPARE(report->items.size(), 4);
        QCOMPARE(report->items.at(0).status, QString("ok"));
        QCOMPARE(report->items.at(1).status, QString("failed"));
        QCOMPARE(report->items.at(1).code, QString("node_failed"));
        QCOMPARE(report->items.at(2).status, QString("failed")); // unreadable: never reached the runner
        QCOMPARE(report->items.at(3).status, QString("ok"));
        QCOMPARE(m_runs, 3);
        QCOMPARE(report->failed(), 2);
        QVERIFY(!QFile::exists(path("out2/b.png")));
        QVERIFY(CanvasStore::forSession().list().empty());
    }

    void cancelStopsAfterThePictureUnderWayAndSaysSo() {
        picture("in3", "a.png");
        picture("in3", "b.png");
        picture("in3", "c.png");
        BatchHooks hooks;
        hooks.cancelled = [this] { return m_runs >= 1; };
        const Result<BatchReport> report = runBatch(spec("in3", "out3"), writes(), hooks);
        QVERIFY(report);
        QCOMPARE(report->ok(), 1);
        QCOMPARE(report->cancelled(), 2);
        QVERIFY(report->items.at(1).error.contains("cancelled"));
        QCOMPARE(m_runs, 1);
    }

    void aRunnerThatCancelsEndsTheBatch() {
        picture("in4", "a.png");
        picture("in4", "b.png");
        const Result<BatchReport> report = runBatch(spec("in4", "out4"), [](const Document &, const QString &) { return BatchRunResult{"cancelled", {}, "Cancelled."}; });
        QVERIFY(report);
        QCOMPARE(report->cancelled(), 2);
        QVERIFY(CanvasStore::forSession().list().empty());
    }

    void anExistingOutputFileIsNeverReplaced() {
        picture("in5", "a.png", mask::Rgba{255, 0, 0, 255});
        QDir().mkpath(path("out5"));
        QFile keep(path("out5/a.png"));
        QVERIFY(keep.open(QIODevice::WriteOnly));
        keep.write("precious");
        keep.close();
        const Result<BatchReport> report = runBatch(spec("in5", "out5"), writes());
        QVERIFY(report);
        QVERIFY(report->items.first().output.endsWith("a-2.png"));
        QFile again(path("out5/a.png"));
        QVERIFY(again.open(QIODevice::ReadOnly));
        QCOMPARE(again.readAll(), QByteArray("precious"));
    }

    void formatAndKeepAreHonoured() {
        picture("in6", "a.png");
        BatchSpec s = spec("in6", "out6");
        s.picture.format = "jpg";
        s.keepCanvases = true;
        const Result<BatchReport> report = runBatch(s, writes());
        QVERIFY(report);
        QVERIFY(report->items.first().output.endsWith("a.jpg"));
        QCOMPARE(int(CanvasStore::forSession().list().size()), 1);
    }

    void theBoundWorkflowTakesTheWholePictureAndThePrompt() {
        picture("in7", "a.png", mask::Rgba{255, 0, 0, 255}, 40, 30);
        BatchSpec s = spec("in7", "out7");
        s.seed = 5;
        Document seen;
        const Result<BatchReport> report = runBatch(s, [&](const Document &bound, const QString &) {
            seen = bound;
            return BatchRunResult{"failed", "x", "stop"};
        });
        QVERIFY(report);
        const Node &region = seen.graph.nodes.value("region");
        QCOMPARE(region.inputs.value("width").literal().toInt(), 40);
        QCOMPARE(region.inputs.value("height").literal().toInt(), 30);
        QVERIFY(!regionCanvas(seen).isEmpty());
        QCOMPARE(seen.graph.nodes.value("encode").inputs.value("prompt").literal().toString(), QString("a hat"));
        QCOMPARE(seen.graph.nodes.value("sample").inputs.value("seed").literal().toInt(), 5);
    }

    void nothingStartsWithoutPicturesOrAPromptOrWithABoundsAction() {
        QDir().mkpath(path("empty"));
        QCOMPARE(runBatch(spec("empty", "oute"), writes()).error().code, QString("input_invalid"));
        QCOMPARE(runBatch(spec("nowhere", "oute"), writes()).error().code, QString("file_not_found"));
        picture("in8", "a.png");
        BatchSpec s = spec("in8", "out8");
        s.prompt.clear();
        QCOMPARE(runBatch(s, writes()).error().code, QString("input_invalid"));
        QCOMPARE(m_runs, 0);
        QFile bounds(path("bounds.fugue"));
        QVERIFY(bounds.open(QIODevice::WriteOnly));
        bounds.write(QByteArray(kAction).replace("\"required\"", "\"bounds\""));
        bounds.close();
        QCOMPARE(loadBatchAction(path("bounds.fugue"), {}, m_registry).error().code, QString("action_invalid"));
        QCOMPARE(loadBatchAction("nonesuch", {}, m_registry).error().code, QString("file_not_found"));
    }
};

QTEST_GUILESS_MAIN(TestCanvasBatch)
#include "test_canvas_batch.moc"
