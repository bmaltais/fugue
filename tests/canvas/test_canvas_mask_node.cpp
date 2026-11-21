// The mask widget on the nodes (#174, spec 14.4): Load Image 1.1 and Paint Mask with the mask editor,
// the mask's undo step, the face with the mask over the picture, and the hollow mask port.
//
// Seam: the real window (Main.qml, offscreen) with CanvasRuns and RunService as main() makes them. A
// picture this test writes feeds a Load Image and a Paint Mask; the editor is opened by a double click
// on Paint Mask's picture and driven by the window's mouse; the mask it saves is read back from its
// file and from the model. With FUGUE_SHOT_DIR set the last test saves the window as pictures.

#include <QImage>
#include <functional>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtTest>

#include "canvas/mask_layer.h"
#include "canvas_app.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

using namespace canvas_test;
using fugue::CanvasRuns;
using fugue::MaskLayer;
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

// load -> paint (Paint Mask) -> invert -> shown; the mask wire between hollow ports is paint -> invert.
QByteArray graphOf(const QString &file, const QString &loadMask = {}) {
    const QString mask = loadMask.isEmpty() ? QString() : QStringLiteral(R"(, "mask": "%1")").arg(loadMask);
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "%1"%2}},
    "paint": {"type": "fugue/paint-mask", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}}},
    "invert": {"type": "fugue/invert-mask", "version": "1.0.1", "inputs": {"mask": {"from": "paint.mask"}}},
    "shown": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "invert.mask"}}}
  }},
  "layout": {"nodes": {"load": {"pos": [0, 0]}, "paint": {"pos": [340, 0]}, "invert": {"pos": [680, 0]}, "shown": {"pos": [1000, 0]}},
             "view": {"center": [420, 150], "zoom": 1}}
})").arg(file, mask).toUtf8();
}

struct MaskApp {
    App app;
    std::unique_ptr<RunService> service;
    std::unique_ptr<CanvasRuns> runs;

    explicit MaskApp(const QByteArray &graph, const Look &look = {}) : app(load(graph), {}, look) {
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
    ~MaskApp() {
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

} // namespace

class TestCanvasMaskNode : public QObject {
    Q_OBJECT

private slots:
    // ---- the model: the mask input and its undo step ----------------------------------------------

    void theMaskInputAndItsStateAreKnown() {
        QTemporaryDir files;
        App app(load(graphOf(write(files.filePath("a.png"), 60, 40))));
        const QVariantMap load = app.model.maskState("load");
        QCOMPARE(load.value("input").toString(), QStringLiteral("mask"));
        QVERIFY(load.value("over").toString().isEmpty()); // over its own file
        QVERIFY(load.value("editable").toBool());
        QCOMPARE(app.model.maskState("paint").value("over").toString(), QStringLiteral("image"));
        QVERIFY(app.model.maskState("shown").isEmpty()); // no mask input
        QVERIFY(app.model.maskInput("invert").isEmpty());
    }

    void settingTheMaskIsOneUndoStepAndUndoGivesNone() {
        QTemporaryDir files;
        App app(load(graphOf(write(files.filePath("a.png"), 60, 40))));
        const QString mask = files.filePath("a-mask.png");
        QImage gray(60, 40, QImage::Format_Grayscale8);
        gray.fill(255);
        QVERIFY(gray.save(mask, "PNG"));
        QCOMPARE(app.model.undoStack()->count(), 0);
        QVERIFY(app.model.setMaskPath("paint", mask).isEmpty());
        QCOMPARE(app.model.undoStack()->count(), 1);
        QCOMPARE(app.model.maskState("paint").value("path").toString(), mask);
        // The same path again is no step; an empty path is none, another step.
        QVERIFY(app.model.setMaskPath("paint", mask).isEmpty());
        QCOMPARE(app.model.undoStack()->count(), 1);
        QVERIFY(app.model.setMaskPath("paint", QString()).isEmpty());
        QVERIFY(app.model.maskState("paint").value("path").toString().isEmpty());
        QCOMPARE(app.model.undoStack()->count(), 2);
        app.model.undoStack()->undo();
        QCOMPARE(app.model.maskState("paint").value("path").toString(), mask);
        app.model.undoStack()->undo();
        QVERIFY(app.model.maskState("paint").value("path").toString().isEmpty());
        QVERIFY(!app.model.setMaskPath("shown", mask).isEmpty()); // a node with no mask says so
    }

    // ---- the window ---------------------------------------------------------------------------------

    void theEditorsSavedSignalSetsTheMaskAsOneStepAndTheFaceShowsIt() {
        QTemporaryDir files;
        MaskApp r(graphOf(write(files.filePath("a.png"), 160, 100)));
        QVERIFY(r.app.open());
        r.run();
        QTRY_VERIFY(r.role("paint", NodeList::Thumbnail).toString().startsWith("file://"));
        QVERIFY(r.byName("maskOverlay") == nullptr); // nothing painted: a plain thumbnail
        // A double click on Paint Mask's picture opens the editor, not the large view.
        QTest::mouseDClick(r.app.window, Qt::LeftButton, {}, r.pictureOf("paint"));
        QObject *editor = nullptr;
        QTRY_VERIFY((editor = r.dialog("maskEditor")) && editor->property("visible").toBool());
        QVERIFY(!r.dialog("imageDialog") || !r.dialog("imageDialog")->property("visible").toBool());
        editor->setProperty("folder", files.filePath("inputs"));
        auto *layer = editor->property("layer").value<MaskLayer *>();
        QTRY_VERIFY(layer->zoom() > 0 && layer->imageSize() == QSize(160, 100));
        editor->setProperty("brushSize", 40);
        editor->setProperty("brushHardness", 1);
        const auto pixel = [&](double x, double y) {
            return layer->mapToScene(QPointF(layer->originX() + x * layer->zoom(), layer->originY() + y * layer->zoom())).toPoint();
        };
        r.app.drag(Qt::LeftButton, pixel(40, 50), pixel(120, 50));
        QVERIFY(!layer->isEmpty());
        const int before = r.steps();
        QTest::mouseClick(r.app.window, Qt::LeftButton, {}, item(r.app.window->contentItem(), "maskOk")->mapToScene(QPointF(10, 10)).toPoint());
        QTRY_VERIFY(!editor->property("visible").toBool());
        QCOMPARE(r.steps(), before + 1); // the whole session is one step
        const QString path = r.app.model.maskState("paint").value("path").toString();
        QVERIFY2(path.startsWith(files.filePath("inputs") + "/a-mask-") || path.contains("-mask-"), qPrintable(path));
        const QImage saved(path);
        QCOMPARE(saved.size(), QSize(160, 100));
        QCOMPARE(saved.format(), QImage::Format_Grayscale8);
        QCOMPARE(qGray(saved.pixel(80, 50)), 255);
        QCOMPARE(qGray(saved.pixel(5, 5)), 0);
        // The face: the picture with the mask over it.
        QTRY_VERIFY(r.byName("maskOverlay") && r.byName("maskOverlay")->isVisible());
        r.shot("paint-mask-painted");
        // A run gives the painted mask on the wire; the node does not run again for nothing else.
        r.run();
        // Undo takes the mask back in that one step.
        r.app.model.undoStack()->undo();
        QVERIFY(r.app.model.maskState("paint").value("path").toString().isEmpty());
        QTRY_VERIFY(r.byName("maskOverlay") == nullptr);
    }

    void loadImagesDoubleClickKeepsTheLargeView() {
        QTemporaryDir files;
        MaskApp r(graphOf(write(files.filePath("a.png"), 160, 100)));
        QVERIFY(r.app.open());
        r.run();
        QTRY_VERIFY(r.role("load", NodeList::Thumbnail).toString().startsWith("file://"));
        QTest::mouseDClick(r.app.window, Qt::LeftButton, {}, r.pictureOf("load"));
        QObject *large = nullptr;
        QTRY_VERIFY((large = r.dialog("imageDialog")) && large->property("visible").toBool());
        QVERIFY(!r.dialog("maskEditor") || !r.dialog("maskEditor")->property("visible").toBool());
    }

    void thePaintMaskRowOpensTheEditorOnLoadImageToo() {
        QTemporaryDir files;
        MaskApp r(graphOf(write(files.filePath("a.png"), 160, 100)));
        QVERIFY(r.app.open());
        r.run();
        QTRY_VERIFY(r.role("load", NodeList::Thumbnail).toString().startsWith("file://"));
        QQuickItem *row = nullptr;
        QQuickItem *face = r.byName("node-load");
        QVERIFY(face);
        QTRY_VERIFY(item(face, "inputWidget"));
        std::function<void(QQuickItem *)> find = [&](QQuickItem *from) {
            for (auto *child : from->childItems()) {
                if (child->objectName() == QLatin1String("inputWidget") && child->property("kind").toString() == QLatin1String("mask"))
                    row = child;
                find(child);
            }
        };
        find(face);
        QVERIFY(row);
        QCOMPARE(row->property("valueText").toString(), QStringLiteral("None"));
        QTest::mouseClick(r.app.window, Qt::LeftButton, {}, row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
        QObject *editor = nullptr;
        QTRY_VERIFY((editor = r.dialog("maskEditor")) && editor->property("visible").toBool());
        QCOMPARE(editor->property("imageSize").toSizeF(), QSizeF(160, 100));
    }

    // ---- the hollow port ----------------------------------------------------------------------------

    void aMaskPortIsAHollowSquareAndAPicturePortIsNot() {
        QTemporaryDir files;
        MaskApp r(graphOf(write(files.filePath("a.png"), 60, 40)));
        QVERIFY(r.app.open());
        QTRY_VERIFY(r.dotOf("paint", "outputDot-mask"));
        for (const auto &[node, dot] : {std::pair{"paint", "outputDot-mask"}, std::pair{"load", "outputDot-mask"},
                                         std::pair{"invert", "inputDot-mask"}, std::pair{"invert", "outputDot-mask"}}) {
            QQuickItem *mark = r.dotOf(node, dot);
            QVERIFY2(mark, dot);
            QVERIFY2(mark->property("mask").toBool(), dot);
            QVERIFY2(!mark->property("solid").toBool(), dot); // hollow even with a wire on it
            QCOMPARE(mark->property("shape").toString(), QStringLiteral("square"));
        }
        // A picture port, wired, is filled.
        QQuickItem *image = r.dotOf("load", "outputDot-image");
        QVERIFY(image);
        QVERIFY(!image->property("mask").toBool());
        QVERIFY(image->property("solid").toBool());
    }

    void saveThePicturesToLookAt() {
        if (qgetenv("FUGUE_SHOT_DIR").isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the nodes' pictures");
        QTemporaryDir files;
        const QString picture = write(files.filePath("a.png"), 320, 200);
        QImage gray(320, 200, QImage::Format_Grayscale8);
        gray.fill(0);
        for (int y = 40; y < 150; ++y)
            for (int x = 90; x < 240; ++x)
                gray.setPixel(x, y, qRgb(255, 255, 255));
        const QString mask = files.filePath("a-mask.png");
        QVERIFY(gray.save(mask, "PNG"));
        MaskApp r(graphOf(picture, mask), Look{"catppuccin", 8});
        QVERIFY(r.app.open());
        r.app.window->resize(1280, 640);
        r.run();
        QVERIFY(r.app.model.setMaskPath("paint", mask).isEmpty());
        QTRY_VERIFY(r.byName("maskOverlay"));
        QTest::qWait(300);
        r.shot("load-and-paint-mask");
    }
};

QTEST_MAIN(TestCanvasMaskNode)
#include "test_canvas_mask_node.moc"
