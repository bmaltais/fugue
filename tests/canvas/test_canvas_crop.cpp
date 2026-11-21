// The large image view and the visual crop (#73, spec 12.7, 14.1).
//
// Seam: the real window (Main.qml, offscreen) with CanvasRuns and RunService as main() makes them.
// A Load Image reads a file this test writes and feeds a Crop and a Preview; a run gives the Preview
// its picture. The model's crop numbers and undo stack are read directly; the frame, the handles
// and the dialogs are driven with the mouse and the keys. The geometry itself is tested apart
// (tests/engine/test_crop_geometry.cpp).

#include <QImage>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtTest>

#include "canvas_app.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

using namespace canvas_test;
using fugue::CanvasRuns;
using fugue::RunService;
using fugue::RunSetup;

namespace {

QString write(const QString &path, int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    image.save(path, "PNG");
    return path;
}

// load -> crop -> shown; load -> raw (a Preview of the whole picture).
QByteArray graphOf(const QString &file, const QString &cropInputs = R"("x": 20, "y": 10, "width": 40, "height": 30)") {
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "load": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "%1"}},
    "crop": {"type": "fugue/crop", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}, %2}},
    "shown": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "crop.image"}}},
    "raw": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "load.image"}}}
  }},
  "layout": {"nodes": {"load": {"pos": [0, 0]}, "crop": {"pos": [300, 0]}, "shown": {"pos": [650, 0]}, "raw": {"pos": [0, 330]}},
             "view": {"center": [500, 200], "zoom": 1}}
})").arg(file, cropInputs).toUtf8();
}

struct RunApp {
    App app;
    std::unique_ptr<RunService> service;
    std::unique_ptr<CanvasRuns> runs;

    explicit RunApp(const QByteArray &graph) : app(load(graph)) {
        RunSetup setup;
        setup.addNodeTypes = fugue::nodes::addBuiltinNodeTypes;
        setup.addKernels = fugue::nodes::addBuiltinKernels;
        setup.registerProviders = [](fugue::Registry &registry) { return registry.setProviders({}); };
        const QString root = app.dir.path();
        setup.runsRoot = root + "/runs";
        setup.queueDir = root + "/queue";
        setup.lockPath = root + "/engine.lock";
        setup.cacheRoot = root + "/results";
        setup.hashesPath = root + "/hashes.json";
        setup.thumbnailDir = root + "/thumbnails";
        setup.collectGarbage = false;
        service = std::make_unique<RunService>(setup);
        runs = std::make_unique<CanvasRuns>(&app.model, &app.workflows, service.get(), &app.registry);
        runs->registerCommands(app.commands);
        app.runs = runs.get();
    }
    ~RunApp() {
        qDeleteAll(app.engine.rootObjects());
        runs.reset();
        service.reset();
    }
    QVariant role(const QString &node, int role) const {
        const NodeList *nodes = app.model.nodes();
        for (int row = 0; row < nodes->rowCount(); ++row)
            if (nodes->data(nodes->index(row), NodeList::NodeId).toString() == node)
                return nodes->data(nodes->index(row), role);
        return {};
    }
    void run() {
        QSignalSpy finished(runs.get(), &CanvasRuns::finished);
        app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));
    }
    QVariantMap rect() const { return app.model.cropRect("crop"); }
    int steps() const { return app.model.undoStack()->count(); }
    QQuickItem *byName(const QString &name) const { return item(app.window->contentItem(), name); }
    QPoint scene(QQuickItem *in, qreal x, qreal y) const { return in->mapToScene(QPointF(x, y)).toPoint(); }
    // The frame's rectangle in the window, from the viewer drawing it.
    QRectF frame(QQuickItem *in = nullptr) const {
        QQuickItem *drawn = item(in ? in : app.window->contentItem(), "cropFrame");
        return drawn ? QRectF(drawn->mapToScene(QPointF(1, 1)), QSizeF(drawn->width() - 2, drawn->height() - 2)) : QRectF();
    }
    QObject *dialog(const char *name) const { return app.window->findChild<QObject *>(name); }
};

} // namespace

class TestCanvasCrop : public QObject {
    Q_OBJECT

private slots:
    // ---- the model: the numbers and their undo steps ---------------------------------------------

    void theNodesNumbersAreAFrameAndWritingThemIsOneUndoStep() {
        QTemporaryDir files;
        App app(load(graphOf(write(files.filePath("a.png"), 100, 60))));
        QCOMPARE(app.model.cropRect("crop").value("x").toInt(), 20);
        QCOMPARE(app.model.cropRect("crop").value("width").toInt(), 40);
        QVERIFY(app.model.cropRect("crop").value("editable").toBool());
        QVERIFY(app.model.cropRect("load").isEmpty()); // not a Crop

        // Two numbers change: one step, named for the node, and only those two are written.
        QVERIFY(app.model.setCropRect("crop", {{"x", 25}, {"y", 10}, {"width", 50}, {"height", 30}}));
        QCOMPARE(app.model.undoStack()->count(), 1);
        QCOMPARE(app.model.cropRect("crop").value("x").toInt(), 25);
        QCOMPARE(app.model.cropRect("crop").value("width").toInt(), 50);
        QCOMPARE(app.model.cropRect("crop").value("y").toInt(), 10);
        // The same numbers again are no edit.
        QVERIFY(!app.model.setCropRect("crop", {{"x", 25}, {"y", 10}, {"width", 50}, {"height", 30}}));
        QCOMPARE(app.model.undoStack()->count(), 1);
        // One undo puts both back.
        app.model.undo();
        QCOMPARE(app.model.cropRect("crop").value("x").toInt(), 20);
        QCOMPARE(app.model.cropRect("crop").value("width").toInt(), 40);
        QVERIFY(!app.model.setCropRect("load", {{"x", 1}}));
    }

    void aNeverSetNumberIsTheNodesDefault() {
        QTemporaryDir files;
        App app(load(graphOf(write(files.filePath("a.png"), 100, 60), R"("x": 3)")));
        const QVariantMap rect = app.model.cropRect("crop");
        QCOMPARE(rect.value("x").toInt(), 3);
        QCOMPARE(rect.value("y").toInt(), 0);
        QCOMPARE(rect.value("width").toInt(), 512);
        QCOMPARE(rect.value("height").toInt(), 512);
    }

    void aLinkedNumberLeavesTheFrameUneditable() {
        QTemporaryDir files;
        const QByteArray graph = QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "n": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 5}},
    "crop": {"type": "fugue/crop", "version": "1.0.0", "inputs": {"x": {"from": "n.value"}}}
  }}
})").toUtf8();
        App app(load(graph));
        QVERIFY(!app.model.cropRect("crop").value("editable").toBool());
        QVERIFY(!app.model.setCropRect("crop", {{"y", 4}}));
        QCOMPARE(app.model.undoStack()->count(), 0);
    }

    // ---- the picture a Crop shows -------------------------------------------------------------------

    void theCropShowsThePictureArrivingAtItsInput() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 100, 60)));
        QVERIFY(r.app.open());
        // Before a run: the file's picture, found by the Load Image feeding it directly.
        QTRY_VERIFY2(r.role("crop", NodeList::Thumbnail).toString().startsWith("file://"), "the input file's preview");
        QCOMPARE(r.role("crop", NodeList::ThumbnailInfo).toString(), QStringLiteral("100 × 60"));
        QCOMPARE(r.runs->imageFile("crop"), files.filePath("a.png"));
        r.run();
        // After one: still the input, not the 40 x 30 result (the Preview after it shows that).
        QCOMPARE(r.role("crop", NodeList::ThumbnailInfo).toString(), QStringLiteral("100 × 60"));
        QTRY_COMPARE(r.role("shown", NodeList::ThumbnailInfo).toString(), QStringLiteral("40 × 30"));
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    void withNothingFeedingItTheNodeSaysSo() {
        const QByteArray graph = QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "crop": {"type": "fugue/crop", "version": "1.0.0", "inputs": {}}}}})").toUtf8();
        RunApp r(graph);
        QVERIFY(r.app.open());
        QCOMPARE(r.role("crop", NodeList::ThumbnailText).toString(), QStringLiteral("No picture yet"));
        QVERIFY(r.role("crop", NodeList::ThumbnailDim).toBool());
        QVERIFY(!r.byName("cropViewer")); // no frame over a placeholder
    }

    // ---- the frame on the node ----------------------------------------------------------------------

    void theFrameShowsWhatTheNodeWillCut() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 100, 60)));
        QVERIFY(r.app.open());
        QTRY_VERIFY(r.byName("cropViewer"));
        QTRY_VERIFY(r.frame().width() > 0);
        QQuickItem *viewer = r.byName("cropViewer");
        Q_UNUSED(viewer);
        // The picture fits the face: pixels to window units is one scale, and the frame is 40 x 30 of them.
        const QRectF frame = r.frame();
        QVERIFY2(qAbs(frame.width() / frame.height() - 40.0 / 30.0) < 0.05, qPrintable(QString::number(frame.width() / frame.height())));
        const qreal scale = frame.width() / 40;
        QVERIFY(scale > 0);
        // Its left edge is 20 pixels in from the picture's.
        QVERIFY(r.byName("cropDimLeft")->width() > 0);
        QVERIFY(qAbs(r.byName("cropDimLeft")->width() - 20 * scale) < 1.5);
        QVERIFY(qAbs(r.byName("cropDimTop")->height() - 10 * scale) < 1.5);
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // One drag, one undo step, however many moves it takes; typing moves the frame too.
    void draggingTheFrameWritesTheNumbersAsOneUndoStep() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 100, 60)));
        QVERIFY(r.app.open());
        QTRY_VERIFY(r.frame().width() > 0);
        const QRectF frame = r.frame();
        const qreal scale = frame.width() / 40;

        // Inside the frame: move it by (10, 5) pixels.
        const QPoint from = frame.center().toPoint();
        r.app.drag(Qt::LeftButton, from, from + QPoint(qRound(10 * scale), qRound(5 * scale)));
        QTRY_COMPARE(r.rect().value("x").toInt(), 30);
        QCOMPARE(r.rect().value("y").toInt(), 15);
        QCOMPARE(r.rect().value("width").toInt(), 40);
        QCOMPARE(r.steps(), 1);
        // The frame follows the numbers.
        QTRY_VERIFY(qAbs(r.frame().left() - frame.left() - 10 * scale) < 1.5);

        // The bottom right handle: 10 pixels out each way.
        const QRectF moved = r.frame();
        r.app.drag(Qt::LeftButton, moved.bottomRight().toPoint(), moved.bottomRight().toPoint() + QPoint(qRound(10 * scale), qRound(10 * scale)));
        QTRY_COMPARE(r.rect().value("width").toInt(), 50);
        QCOMPARE(r.rect().value("height").toInt(), 40);
        QCOMPARE(r.rect().value("x").toInt(), 30);
        QCOMPARE(r.steps(), 2);

        // A drag past the picture's edge stops at the edge.
        const QRectF big = r.frame();
        // (To the window's own edge: further is outside it.)
        r.app.drag(Qt::LeftButton, big.bottomRight().toPoint(), QPoint(r.app.window->width() - 2, r.app.window->height() - 2));
        QTRY_COMPARE(r.rect().value("width").toInt(), 70);  // 100 - 30
        QCOMPARE(r.rect().value("height").toInt(), 45);     // 60 - 15
        QCOMPARE(r.steps(), 3);

        // Undo takes one drag back at a time.
        r.app.model.undo();
        QCOMPARE(r.rect().value("width").toInt(), 50);
        r.app.model.undo();
        QCOMPARE(r.rect().value("width").toInt(), 40);
        QCOMPARE(r.rect().value("x").toInt(), 30);
        // A press that is not on a handle or in the frame writes nothing (it pans, which at Fit stays put).
        const int steps = r.steps();
        r.app.drag(Qt::LeftButton, r.scene(r.byName("cropMouse"), 2, 2), r.scene(r.byName("cropMouse"), 12, 12));
        QCOMPARE(r.steps(), steps);
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // ---- the large views ----------------------------------------------------------------------------

    void aDoubleClickOnAnyPictureOpensItLarge() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 100, 60)));
        QVERIFY(r.app.open());
        r.run();
        // The raw Preview's picture, at its node's place.
        QTRY_VERIFY(r.role("raw", NodeList::Thumbnail).toString().startsWith("file://"));
        const QRectF slot = r.role("raw", NodeList::ThumbnailRect).toRectF();
        const QPoint at = r.app.screen(slot.center().x(), 330 + slot.center().y());
        QTest::mouseDClick(r.app.window, Qt::LeftButton, {}, at);
        QObject *popup = r.dialog("imageDialog");
        QTRY_VERIFY(popup && popup->property("visible").toBool());
        QObject *view = popup->property("viewer").value<QObject *>();
        QCOMPARE(view->property("sourceA").toUrl(), QUrl::fromLocalFile(files.filePath("a.png")));
        QCOMPARE(view->property("sizeA").toSizeF(), QSizeF(100, 60));
        QVERIFY(view->property("sizeNotice").toString().contains("100 × 60"));
        QVERIFY(view->property("sizeNotice").toString().contains("%"));

        // The wheel zooms at the pointer; Fit and 100% (and 1, 0) answer.
        QQuickItem *viewer = static_cast<QQuickItem *>(view);
        QTRY_VERIFY(viewer->width() > 100);
        const QPointF mid(viewer->width() / 2, viewer->height() / 2);
        QWheelEvent wheel(mid + QPointF(viewer->mapToScene(QPointF(0, 0))), mid, QPoint(), QPoint(0, 240), Qt::NoButton, {}, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(r.app.window, &wheel);
        QTRY_VERIFY(view->property("zoom").toReal() > 1);
        QTest::keyClick(r.app.window, Qt::Key_1);
        QTRY_VERIFY(view->property("actual").toBool());
        QTest::keyClick(r.app.window, Qt::Key_0);
        QTRY_VERIFY(!view->property("actual").toBool());
        QCOMPARE(view->property("zoom").toReal(), 1.0);
        QTest::keyClick(r.app.window, Qt::Key_Escape);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(r.steps(), 0);
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    void theCropWindowEditsADraftAndOKWritesIt() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 100, 60)));
        QVERIFY(r.app.open());
        QTRY_VERIFY(r.frame().width() > 0);
        // A double click inside the frame opens the crop window.
        QTest::mouseDClick(r.app.window, Qt::LeftButton, {}, r.frame().center().toPoint());
        QObject *popup = r.dialog("cropDialog");
        QTRY_VERIFY(popup && popup->property("visible").toBool());
        QCOMPARE(popup->property("draft").toMap().value("width").toInt(), 40);

        // The aspect lock: 1:1 fits the frame (40 x 30) to 30 x 30 around its middle.
        QQuickItem *square = r.byName("cropRatio-1:1");
        QVERIFY(square);
        QTest::mouseClick(r.app.window, Qt::LeftButton, {}, r.scene(square, square->width() / 2, square->height() / 2));
        QTRY_COMPARE(popup->property("draft").toMap().value("width").toInt(), 30);
        QCOMPARE(popup->property("draft").toMap().value("height").toInt(), 30);
        QCOMPARE(popup->property("draft").toMap().value("x").toInt(), 25);
        // Nothing is written until OK.
        QCOMPARE(r.rect().value("width").toInt(), 40);
        QCOMPARE(r.steps(), 0);

        // A drag of the frame's corner keeps the lock.
        const QRectF frame = r.frame(r.byName("cropView"));
        QVERIFY(frame.width() > 0);
        r.app.drag(Qt::LeftButton, frame.bottomRight().toPoint(), frame.bottomRight().toPoint() + QPoint(30, 4));
        QTRY_VERIFY(popup->property("draft").toMap().value("width").toInt() > 30);
        const QVariantMap draft = popup->property("draft").toMap();
        QCOMPARE(draft.value("width").toInt(), draft.value("height").toInt());
        QCOMPARE(r.steps(), 0);

        // A typed width resizes within the lock.
        QQuickItem *field = r.byName("cropField-width");
        QVERIFY(field);
        field->forceActiveFocus();
        QTest::keyClick(r.app.window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(r.app.window, Qt::Key_2);
        QTest::keyClick(r.app.window, Qt::Key_0);
        QTest::keyClick(r.app.window, Qt::Key_Tab); // leaving the field enters it
        QTRY_COMPARE(popup->property("draft").toMap().value("width").toInt(), 20);
        QCOMPARE(popup->property("draft").toMap().value("height").toInt(), 20);

        // OK writes the four numbers as one step and closes.
        QQuickItem *ok = r.byName("cropOk");
        QVERIFY(ok);
        QTest::mouseClick(r.app.window, Qt::LeftButton, {}, r.scene(ok, ok->width() / 2, ok->height() / 2));
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(r.steps(), 1);
        QCOMPARE(r.rect().value("width").toInt(), 20);
        QCOMPARE(r.rect().value("height").toInt(), 20);
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    void cancelAndEscapeLeaveTheNodeAsItWas() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 100, 60)));
        QVERIFY(r.app.open());
        QTRY_VERIFY(r.frame().width() > 0);
        for (const bool escape : {false, true}) {
            QTest::mouseDClick(r.app.window, Qt::LeftButton, {}, r.frame().center().toPoint());
            QObject *popup = r.dialog("cropDialog");
            QTRY_VERIFY(popup && popup->property("visible").toBool());
            QVERIFY(QMetaObject::invokeMethod(popup, "pickRatio", Q_ARG(QVariant, QVariant(QStringLiteral("16:9")))));
            QVERIFY(popup->property("draft").toMap().value("height").toInt() != 30);
            if (escape) {
                QTest::keyClick(r.app.window, Qt::Key_Escape);
            } else {
                QQuickItem *cancel = r.byName("cropCancel");
                QTest::mouseClick(r.app.window, Qt::LeftButton, {}, r.scene(cancel, cancel->width() / 2, cancel->height() / 2));
            }
            QTRY_VERIFY(!popup->property("visible").toBool());
            QCOMPARE(r.steps(), 0);
            QCOMPARE(r.rect().value("width").toInt(), 40);
            QCOMPARE(r.rect().value("height").toInt(), 30);
        }
    }

    void enterAppliesTheCropWindow() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 100, 60)));
        QVERIFY(r.app.open());
        QTRY_VERIFY(r.frame().width() > 0);
        QTest::mouseDClick(r.app.window, Qt::LeftButton, {}, r.frame().center().toPoint());
        QObject *popup = r.dialog("cropDialog");
        QTRY_VERIFY(popup && popup->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(popup, "pickRatio", Q_ARG(QVariant, QVariant(QStringLiteral("1:1")))));
        QTest::keyClick(r.app.window, Qt::Key_Return);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(r.rect().value("width").toInt(), 30);
        QCOMPARE(r.steps(), 1);
    }
};

QTEST_MAIN(TestCanvasCrop)
#include "test_canvas_crop.moc"
