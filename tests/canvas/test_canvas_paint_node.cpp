// The paint widget on the Paint node (#181, spec 14.4): the window the widget row opens, the layers it
// saves as one .ora file, the node's state and undo step, and the result a run gives.
//
// Seam: the real window (Main.qml, offscreen) with CanvasRuns and RunService as main() makes them. The
// widget row is clicked as a person does; the window (a MaskEditor with `layers`) is driven by the
// window's mouse and its buttons; the file it saves is read back with the engine's own reader. With
// FUGUE_SHOT_DIR set, the last test saves the open window, light and dark, as pictures to look at.

#include <QImage>
#include <functional>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtTest>

#include "canvas/paint_layer.h"
#include "canvas_app.h"
#include "engine/paint_file.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

using namespace canvas_test;
using fugue::CanvasRuns;
using fugue::PaintLayer;
using fugue::RunService;
using fugue::RunSetup;

namespace {

QString write(const QString &path, int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, qRgb(40 + x * 180 / width, 90 + y * 100 / height, 150 - x * 90 / width));
    image.save(path, "PNG");
    return path;
}

// A Paint node on a picture (load -> paint -> shown), or on a blank canvas of 200 x 120.
QByteArray graphOf(const QString &file, const QString &paint = {}) {
    const QString painted = paint.isEmpty() ? QString() : QStringLiteral(R"(, "paint": "%1")").arg(paint);
    const QString source = file.isEmpty() ? QStringLiteral(R"("width": 200, "height": 120)") : QStringLiteral(R"("image": {"from": "load.image"})");
    const QString load = file.isEmpty() ? QString()
                                        : QStringLiteral(R"("load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "%1"}},)").arg(file);
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {
    %1
    "paint": {"type": "fugue/paint", "version": "1.0.0", "inputs": {%2%3}},
    "shown": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "paint.image"}}}
  }},
  "layout": {"nodes": {"load": {"pos": [0, 0]}, "paint": {"pos": [340, 0]}, "shown": {"pos": [700, 0]}},
             "view": {"center": [420, 150], "zoom": 1}}
})").arg(load, source, painted).toUtf8();
}

struct PaintApp {
    App app;
    std::unique_ptr<RunService> service;
    std::unique_ptr<CanvasRuns> runs;

    explicit PaintApp(const QByteArray &graph, const Look &look = {}) : app(load(graph), {}, look) {
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
    ~PaintApp() {
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
    QQuickItem *byName(const QString &name) const { return item(app.window->contentItem(), name); }
    QObject *dialog(const char *name) const { return app.window->findChild<QObject *>(name); }
    // The picture slot of a node, in window coordinates.
    QPoint pictureOf(const QString &node) const {
        const QRectF slot = role(node, NodeList::ThumbnailRect).toRectF();
        const QPointF pos = app.model.position(node);
        return app.screen(pos.x() + slot.center().x(), pos.y() + slot.center().y());
    }
    int steps() const { return app.model.undoStack()->count(); }
    // The first PortDot of the node named `name` among the window's items (a node's dots are named
    // "inputDot-<input>" and "outputDot-<output>"); the node's own subtree is searched.
    QQuickItem *dotOf(const QString &node, const QString &name) const {
        QQuickItem *face = byName("node-" + node);
        return face ? item(face, name) : nullptr;
    }
    void shot(const QString &name) const {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (!dir.isEmpty())
            test_support::grab(app.window).save(QString::fromUtf8(dir) + "/" + name + ".png");
    }
};

// The widget row of the paint input among the window's items.
QQuickItem *paintRow(QQuickItem *from) {
    QQuickItem *found = nullptr;
    std::function<void(QQuickItem *)> find = [&](QQuickItem *at) {
        for (auto *child : at->childItems()) {
            if (child->objectName() == QLatin1String("inputWidget") && child->property("kind").toString() == QLatin1String("paint"))
                found = child;
            find(child);
        }
    };
    find(from);
    return found;
}

} // namespace

class TestCanvasPaintNode : public QObject {
    Q_OBJECT

    // Opens the window by the widget row's click, as a person does; the layer to look at and drive.
    struct Window {
        QObject *editor = nullptr;
        PaintLayer *layer = nullptr;
        PaintApp &r;
        explicit Window(PaintApp &app, const QString &folder) : r(app) {
            QQuickItem *face = r.byName("node-paint");
            QQuickItem *row = nullptr;
            QTRY_VERIFY_WITH_TIMEOUT((row = paintRow(face)) != nullptr, 5000);
            QTest::mouseClick(r.app.window, Qt::LeftButton, {}, row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
            QTRY_VERIFY_WITH_TIMEOUT((editor = r.dialog("paintEditor")) && editor->property("visible").toBool(), 5000);
            editor->setProperty("folder", folder);
            layer = editor->property("paintCanvas").value<PaintLayer *>();
            QTRY_VERIFY(layer && layer->zoom() > 0);
        }
        QPoint at(double x, double y) const {
            return layer->mapToScene(QPointF(layer->originX() + x * layer->zoom(), layer->originY() + y * layer->zoom())).toPoint();
        }
        QPoint center(const QString &name) const {
            QQuickItem *it = r.byName(name);
            return it ? it->mapToScene(QPointF(it->width() / 2, it->height() / 2)).toPoint() : QPoint();
        }
        void press(const QString &name) { QTest::mouseClick(r.app.window, Qt::LeftButton, {}, center(name)); }
        void drag(QPointF from, QPointF to) { r.app.drag(Qt::LeftButton, at(from.x(), from.y()), at(to.x(), to.y())); }
    };

private slots:
    // ---- the model: the paint input and its undo step ---------------------------------------------

    void thePaintInputAndItsStateAreKnown() {
        {
            App app(load(graphOf({})));
            const QVariantMap blank = app.model.paintState("paint");
            QCOMPARE(blank.value("input").toString(), QStringLiteral("paint"));
            QCOMPARE(blank.value("over").toString(), QStringLiteral("image"));
            QVERIFY(blank.value("editable").toBool());
            QVERIFY(!blank.value("linked").toBool());
            QCOMPARE(blank.value("width").toInt(), 200);
            QCOMPARE(blank.value("height").toInt(), 120);
            QVERIFY(app.model.paintState("shown").isEmpty());
            QVERIFY(app.model.paintInput("shown").isEmpty());
        }
        QTemporaryDir files;
        App app(load(graphOf(write(files.filePath("a.png"), 60, 40))));
        const QVariantMap linked = app.model.paintState("paint");
        QVERIFY(linked.value("linked").toBool());
        QCOMPARE(linked.value("width").toInt(), 0);
    }

    void setPaintPathIsOneUndoStepAndRefusesANodeWithNone() {
        App app(load(graphOf({})));
        QVERIFY(app.model.setPaintPath("paint", "inputs/p.ora").isEmpty());
        QCOMPARE(app.model.paintState("paint").value("path").toString(), QStringLiteral("inputs/p.ora"));
        QCOMPARE(app.model.undoStack()->count(), 1);
        QVERIFY(app.model.setPaintPath("paint", "inputs/p.ora").isEmpty()); // the same: no step
        QCOMPARE(app.model.undoStack()->count(), 1);
        QVERIFY(app.model.setPaintPath("paint", QString()).isEmpty());
        app.model.undoStack()->undo();
        QCOMPARE(app.model.paintState("paint").value("path").toString(), QStringLiteral("inputs/p.ora"));
        QVERIFY(!app.model.setPaintPath("shown", "x.ora").isEmpty());
    }

    // ---- the window --------------------------------------------------------------------------------

    void theRowOpensTheWindowOnABlankCanvasAndOkSavesOneStepThatARunUses() {
        QTemporaryDir files;
        PaintApp r(graphOf({}));
        QVERIFY(r.app.open());
        Window w(r, files.filePath("inputs"));
        QCOMPARE(w.layer->imageSize(), QSize(200, 120));
        QVERIFY(w.layer->checker()); // no picture: a checkerboard stands under it
        QVERIFY(w.layer->isEmpty());
        // The layers panel: a picture (none) and the paint layer, the active one on top.
        QVERIFY(r.byName("paintLayersPanel")->isVisible());
        QCOMPARE(w.layer->layers().size(), 2);
        w.editor->setProperty("color", QColor(220, 30, 30));
        w.editor->setProperty("brushSize", 30);
        w.editor->setProperty("brushHardness", 1);
        w.drag({40, 60}, {160, 60});
        QCOMPARE(w.layer->colorAt(100, 60), QColor(220, 30, 30));
        const int before = r.steps();
        w.press("maskOk");
        QTRY_VERIFY(!w.editor->property("visible").toBool());
        QCOMPARE(r.steps(), before + 1); // the whole session is one step
        const QString path = r.app.model.paintState("paint").value("path").toString();
        QVERIFY2(path.startsWith(files.filePath("inputs") + "/") && path.endsWith(".ora"), qPrintable(path));
        const auto saved = fugue::mask::readPaintFile(path);
        QVERIFY(saved);
        QCOMPARE(saved->width(), 200);
        QCOMPARE(saved->flatten().rgbaAt(100, 60), (fugue::mask::Rgba{220, 30, 30, 255}));
        // The face says so, and a run gives the picture with the paint on it.
        QCOMPARE(r.role("paint", NodeList::Inputs).toList().isEmpty(), false);
        r.run();
        const QString result = r.runs->imageFile("paint");
        QVERIFY(!result.isEmpty());
        const QImage image(result);
        QCOMPARE(image.size(), QSize(200, 120));
        QCOMPARE(QColor(image.pixel(100, 60)).red(), 220);
        QCOMPARE(qAlpha(image.pixel(5, 5)), 0);
        // Undo takes the painting back in that one step.
        r.app.model.undoStack()->undo();
        QVERIFY(r.app.model.paintState("paint").value("path").toString().isEmpty());
    }

    void cancelAndAnEmptyCanvasChangeNothing() {
        QTemporaryDir files;
        PaintApp r(graphOf({}));
        QVERIFY(r.app.open());
        {
            Window w(r, files.filePath("inputs"));
            w.drag({40, 60}, {160, 60});
            QVERIFY(!w.layer->isEmpty());
            const int before = r.steps();
            w.press("maskCancel");
            QTRY_VERIFY(!w.editor->property("visible").toBool());
            QCOMPARE(r.steps(), before);
            QVERIFY(r.app.model.paintState("paint").value("path").toString().isEmpty());
        }
        Window again(r, files.filePath("inputs"));
        QVERIFY(again.layer->isEmpty()); // each open starts from the stored file
        const int before = r.steps();
        again.press("maskOk"); // nothing painted: no file, and no step
        QTRY_VERIFY(!again.editor->property("visible").toBool());
        QCOMPARE(r.steps(), before);
        QVERIFY(!QDir(files.filePath("inputs")).exists());
    }

    void thePanelAddsHidesMovesAndRemovesLayersAndEachIsUndoable() {
        QTemporaryDir files;
        PaintApp r(graphOf({}));
        QVERIFY(r.app.open());
        Window w(r, files.filePath("inputs"));
        w.press("paintLayerAdd");
        QCOMPARE(w.layer->layers().size(), 3);
        w.press("paintLayerAddMask");
        QCOMPARE(w.layer->layers().size(), 4);
        QCOMPARE(w.layer->layers().first().toMap().value("kind").toString(), QString("mask"));
        QVERIFY(w.layer->canUndo());
        // The eye of the top layer hides it; a second click shows it.
        w.press("paintLayerEye-0");
        QVERIFY(!w.layer->layers().first().toMap().value("visible").toBool());
        w.press("paintLayerEye-0");
        QVERIFY(w.layer->layers().first().toMap().value("visible").toBool());
        // Down, then remove: the mask layer goes under the layer, then away.
        w.press("paintLayerDown");
        QCOMPARE(w.layer->layers().at(1).toMap().value("kind").toString(), QString("mask"));
        w.press("paintLayerRemove");
        QCOMPARE(w.layer->layers().size(), 3);
        w.editor->setProperty("tool", "brush");
        QTest::keyClick(r.app.window, Qt::Key_Z, Qt::ControlModifier); // the remove
        QCOMPARE(w.layer->layers().size(), 4);
        // The picture's row cannot be removed.
        QVERIFY(w.layer->selectLayer(w.layer->layers().last().toMap().value("id").toInt()));
        QTRY_VERIFY(!r.byName("paintLayerRemove")->isEnabled());
        QVERIFY(!w.layer->canPaint());
        w.press("maskCancel");
    }

    void theColorSwatchOpensTheSharedPickerAndItsPickIsTheBrushColor() {
        QTemporaryDir files;
        PaintApp r(graphOf({}));
        QVERIFY(r.app.open());
        Window w(r, files.filePath("inputs"));
        w.press("paintColor");
        QObject *popup = nullptr;
        QTRY_VERIFY((popup = r.dialog("colorPickerPopup")) && popup->property("visible").toBool());
        QMetaObject::invokeMethod(popup->property("pane").value<QObject *>(), "setColor", Q_ARG(QVariant, QString("#2040ff")));
        QTRY_VERIFY(r.byName("colorOk"));
        QTest::mouseClick(r.app.window, Qt::LeftButton, {}, w.r.byName("colorOk")->mapToScene(QPointF(10, 10)).toPoint());
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(w.editor->property("color").value<QColor>(), QColor("#2040ff"));
        QCOMPARE(w.layer->color(), QColor("#2040ff"));
        w.press("maskCancel");
    }

    void aPictureFromANodeThatHasNotRunSaysSoAndOpensNothingUntilItHas() {
        PaintApp r(QByteArray(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "base": {"type": "fugue/paint", "version": "1.0.0", "inputs": {"width": 90, "height": 60}},
    "paint": {"type": "fugue/paint", "version": "1.0.0", "inputs": {"image": {"from": "base.image"}}},
    "shown": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "paint.image"}}}
  }},
  "layout": {"nodes": {"base": {"pos": [0, 0]}, "paint": {"pos": [340, 0]}, "shown": {"pos": [700, 0]}}, "view": {"center": [420, 150], "zoom": 1}}
})"));
        QVERIFY(r.app.open());
        QQuickItem *row = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((row = paintRow(r.byName("node-paint"))) != nullptr, 5000);
        const QPoint at = row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint();
        QTest::mouseClick(r.app.window, Qt::LeftButton, {}, at);
        QTest::qWait(200);
        QObject *editor = r.dialog("paintEditor");
        QVERIFY(!editor || !editor->property("visible").toBool()); // the notice says why; there is no picture yet
        r.run();
        QTest::mouseClick(r.app.window, Qt::LeftButton, {}, at);
        QTRY_VERIFY_WITH_TIMEOUT((editor = r.dialog("paintEditor")) && editor->property("visible").toBool(), 5000);
        QCOMPARE(editor->property("imageSize").toSizeF(), QSizeF(90, 60));
        QVERIFY(editor->property("pictureFile").toString().endsWith(".png"));
    }

    void theWindowOnAPictureDrawsItPaintsOverItAndAScaledPaintingWarns() {
        QTemporaryDir files;
        PaintApp r(graphOf(write(files.filePath("a.png"), 160, 100)));
        QVERIFY(r.app.open());
        r.run();
        Window w(r, files.filePath("inputs"));
        QCOMPARE(w.layer->imageSize(), QSize(160, 100));
        QVERIFY(w.layer->hasPicture());
        QVERIFY(!w.layer->checker());
        QCOMPARE(w.layer->colorAt(2, 2), QColor(qRgb(40 + 2 * 180 / 160, 90 + 2 * 100 / 100, 150 - 2 * 90 / 160)).toRgb());
        w.editor->setProperty("color", QColor(255, 255, 255));
        w.editor->setProperty("brushSize", 20);
        w.editor->setProperty("brushHardness", 1);
        w.drag({30, 50}, {130, 50});
        w.press("maskOk");
        QTRY_VERIFY(!w.editor->property("visible").toBool());
        const QString path = r.app.model.paintState("paint").value("path").toString();
        QVERIFY(path.endsWith(".ora"));
        r.run();
        const QImage image(r.runs->imageFile("paint"));
        QCOMPARE(image.size(), QSize(160, 100));
        QCOMPARE(QColor(image.pixel(80, 50)), QColor(255, 255, 255));
        QVERIFY(QColor(image.pixel(5, 5)) != QColor(255, 255, 255)); // the picture shows elsewhere
        // The window again starts from what was saved, and the picture's row says a picture is there.
        Window again(r, files.filePath("inputs"));
        QCOMPARE(again.layer->colorAt(80, 50), QColor(255, 255, 255));
        QVERIFY(again.layer->warning().isEmpty());
        again.press("maskCancel");
    }

    // ---- pictures to look at (not checks) ----------------------------------------------------------------

    void savesTheWindowOnLightAndDarkThemes_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("name");
        QTest::newRow("dark") << "tokyo-night" << "dark";
        QTest::newRow("light") << "catppuccin-latte" << "light";
    }
    void savesTheWindowOnLightAndDarkThemes() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the paint window's pictures");
        QFETCH(QString, theme);
        QFETCH(QString, name);
        QTemporaryDir files;
        PaintApp r(graphOf(write(files.filePath("a.png"), 640, 400)), Look{theme, 8});
        QVERIFY(r.app.open());
        r.app.window->resize(1400, 900);
        r.run();
        Window w(r, files.filePath("inputs"));
        w.editor->setProperty("brushSize", 60);
        w.editor->setProperty("brushHardness", 0.7);
        w.editor->setProperty("color", QColor(230, 60, 40));
        w.drag({120, 120}, {300, 170});
        w.press("paintLayerAdd");
        w.editor->setProperty("color", QColor(250, 210, 40));
        w.drag({300, 170}, {480, 120});
        w.press("paintLayerAddMask");
        w.editor->setProperty("brushSize", 40);
        w.drag({200, 300}, {440, 300});
        QVERIFY(w.layer->setLayerName(w.layer->layers().at(1).toMap().value("id").toInt(), "Sun"));
        QVERIFY(w.layer->setLayerVisible(w.layer->layers().at(2).toMap().value("id").toInt(), false));
        QTest::mouseMove(r.app.window, w.at(520, 330));
        QTest::qWait(300);
        const auto save = [&](const QString &suffix) {
            const QImage shot = test_support::grab(r.app.window);
            QVERIFY(!shot.isNull());
            QVERIFY(shot.save(QStringLiteral("%1/paint-window-%2-%3.png").arg(QString::fromUtf8(dir), name, suffix)));
        };
        save("layers");
        w.press("paintColor");
        QTest::qWait(300);
        save("color-picker");
    }
};

QTEST_MAIN(TestCanvasPaintNode)
#include "test_canvas_paint_node.moc"
