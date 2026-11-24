// Compare Images in the real window (#62, spec 14.1, 12.7).
//
// Seam: the window (Main.qml, offscreen) with CanvasRuns, RunService and ImageActions as main()
// makes them. Two Load Image nodes read files this test writes; a run gives the node its
// pictures; the viewer is driven with the mouse and the commands, and its state is read from
// the canvas model (compareView), the files it shows from the node's `compare` role.

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtTest>

#include "canvas_app.h"
#include "menu/image_actions.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

using namespace canvas_test;
using fugue::CanvasRuns;
using fugue::RunService;
using fugue::RunSetup;

namespace {

QString write(const QString &path, int width, int height, QColor color) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    image.save(path, "PNG");
    return path;
}

QByteArray graphOf(const QString &a, const QString &b) {
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "left": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "%1"}},
    "right": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "%2"}},
    "cmp": {"type": "fugue/compare-images", "version": "1.0.0",
            "inputs": {"a": {"from": "left.image"}, "b": {"from": "right.image"}}}
  }},
  "layout": {"nodes": {"left": {"pos": [0, 0]}, "right": {"pos": [0, 300]}, "cmp": {"pos": [300, 0]}},
             "view": {"center": [500, 200], "zoom": 1}}
})").arg(a, b).toUtf8();
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
    // Runs the workflow and waits for it; the node's pictures are in its `compare` role.
    void run() {
        QSignalSpy finished(runs.get(), &CanvasRuns::finished);
        app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));
    }
    QVariantMap compare() const { return role("cmp", NodeList::Compare).toMap(); }
    QString file(const char *which) const { return compare().value(which).toMap().value("source").toString(); }
    QQuickItem *viewer() const { return item(app.window->contentItem(), "compareViewer"); }
};

// The window with the image actions of main().
struct ActionApp : RunApp {
    fugue::ImageActions actions;
    explicit ActionApp(const QByteArray &graph)
        : RunApp(graph), actions(app.commands, app.model, *runs, [](const QUrl &) { return true; }) {
        app.imageActions = &actions;
    }
};

bool allBlack(const QImage &image) {
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qRed(image.pixel(x, y)) || qGreen(image.pixel(x, y)) || qBlue(image.pixel(x, y)))
                return false;
    return !image.isNull();
}

} // namespace

class TestCanvasCompare : public QObject {
    Q_OBJECT

private slots:
    // Two fake images, a run: the node holds A, B, the difference and the composite as files,
    // and the labels name what made A and B.
    void aRunGivesTheNodeItsPictures() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 16, 10, Qt::red), write(files.filePath("b.png"), 16, 10, Qt::blue)));
        QVERIFY(r.app.open());
        r.run();
        for (const char *which : {"a", "b", "difference", "composite"})
            QVERIFY2(QFileInfo(r.file(which)).isFile(), which);
        QCOMPARE(r.compare().value("a").toMap().value("width").toInt(), 16);
        QCOMPARE(r.compare().value("labelA").toString(), QStringLiteral("Load Image"));
        // The slot is A's size; the full files are the ones the viewer's large view reads.
        QVERIFY(r.compare().value("a").toMap().value("fileUrl").toString().startsWith("file://"));
        QVERIFY(r.role("cmp", NodeList::Thumbnail).toString().startsWith("file://"));
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    void identicalImagesGiveABlackDifferenceAndDifferentOnesDoNot() {
        QTemporaryDir files;
        const QString a = write(files.filePath("a.png"), 16, 10, QColor(40, 90, 200));
        {
            RunApp r(graphOf(a, a));
            QVERIFY(r.app.open());
            r.run();
            QVERIFY(allBlack(QImage(r.file("difference"))));
        }
        RunApp r(graphOf(a, write(files.filePath("b.png"), 16, 10, QColor(40, 90, 210))));
        QVERIFY(r.app.open());
        r.run();
        const QImage difference(r.file("difference"));
        QVERIFY(!allBlack(difference));
        QCOMPARE(qBlue(difference.pixel(3, 3)), 40); // |200 - 210| times the gain of 4
        QCOMPARE(qRed(difference.pixel(3, 3)), 0);
    }

    // The viewer is on the node's face; the divider follows the pointer and a drag, flip and
    // the mode key change the node's state, none of it is an edit.
    void theViewerWipesFlipsAndCyclesWithoutAnEdit() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 16, 10, Qt::red), write(files.filePath("b.png"), 16, 10, Qt::blue)));
        QVERIFY(r.app.open());
        r.run();
        QTRY_VERIFY(r.viewer());
        QQuickItem *view = r.viewer();
        QQuickItem *mouse = item(view, "compareMouse");
        QVERIFY(mouse);
        QCOMPARE(r.app.model.compareView("cmp").value("mode").toString(), QStringLiteral("slider")); // the primary
        QCOMPARE(r.app.model.compareView("cmp").value("position").toReal(), 0.5);

        // Hover moves the divider; a drag does too.
        const QPointF origin = mouse->mapToScene(QPointF(0, 0));
        const auto at = [&](qreal fraction) { return (origin + QPointF(mouse->width() * fraction, mouse->height() / 2)).toPoint(); };
        QTest::mouseMove(r.app.window, at(0.25));
        QTRY_COMPARE_WITH_TIMEOUT(qRound(r.app.model.compareView("cmp").value("position").toReal() * 100), 25, 2000);
        QTest::mousePress(r.app.window, Qt::LeftButton, {}, at(0.25));
        QTest::mouseMove(r.app.window, at(0.6));
        QTest::mouseMove(r.app.window, at(0.75));
        QTest::mouseRelease(r.app.window, Qt::LeftButton, {}, at(0.75));
        QTRY_COMPARE_WITH_TIMEOUT(qRound(r.app.model.compareView("cmp").value("position").toReal() * 100), 75, 2000);
        QCOMPARE(r.app.model.selectedIds(), QStringList{"cmp"}); // a press selects the node
        // The divider is where the model says.
        QQuickItem *divider = item(view, "divider");
        QVERIFY(divider);
        QTRY_VERIFY(qAbs(divider->x() - view->width() * 0.75) < 3);

        QVERIFY(r.app.commands.contextActive("compare"));
        QVERIFY(r.app.commands.trigger("compare.flip"));
        QCOMPARE(r.app.model.compareView("cmp").value("mode").toString(), QStringLiteral("flip"));
        QVERIFY(!r.app.model.compareView("cmp").value("showB").toBool());
        QVERIFY(r.app.commands.trigger("compare.flip"));
        QVERIFY(r.app.model.compareView("cmp").value("showB").toBool());
        QTRY_VERIFY(item(view, "layerB")->isVisible() && !item(view, "layerA")->isVisible());
        QVERIFY(r.app.commands.trigger("compare.next-mode")); // flip -> difference
        QCOMPARE(r.app.model.compareView("cmp").value("mode").toString(), QStringLiteral("difference"));
        QTRY_VERIFY(item(view, "layerDifference")->isVisible());
        QVERIFY(r.app.commands.trigger("compare.next-mode")); // -> slider again
        QCOMPARE(r.app.model.compareView("cmp").value("mode").toString(), QStringLiteral("slider"));
        QVERIFY(r.app.commands.trigger("compare.actual"));
        QVERIFY(r.app.model.compareView("cmp").value("actual").toBool());
        QVERIFY(r.app.commands.trigger("compare.fit"));
        QVERIFY(!r.app.model.compareView("cmp").value("actual").toBool());

        QCOMPARE(r.app.model.undoStack()->count(), 0); // view state is not an edit
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // A double click on the picture opens the large view on the full files; it shares the
    // node's state, and Escape closes it.
    void aDoubleClickOpensTheLargeViewOnTheFullFiles() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 16, 10, Qt::red), write(files.filePath("b.png"), 8, 5, Qt::blue)));
        QVERIFY(r.app.open());
        r.run();
        QTRY_VERIFY(r.viewer());
        QQuickItem *mouse = item(r.viewer(), "compareMouse");
        QTest::mouseDClick(r.app.window, Qt::LeftButton, {}, mouse->mapToScene(QPointF(mouse->width() / 2, mouse->height() / 2)).toPoint());
        QObject *popup = r.app.window->findChild<QObject *>("compareDialog");
        QTRY_VERIFY(popup && popup->property("visible").toBool());
        QObject *large = popup->property("viewer").value<QObject *>();
        QVERIFY(large);
        QCOMPARE(large->property("sourceA").toUrl(), QUrl::fromLocalFile(r.file("a")));
        QCOMPARE(large->property("sourceB").toUrl(), QUrl::fromLocalFile(r.file("b")));
        // B is smaller than A: the notice says so (sizes shown, B fitted to A).
        QVERIFY(large->property("differs").toBool());
        QVERIFY(large->property("sizeNotice").toString().contains("fitted to A"));
        // The state is the node's: a mode picked in the dialog is the node's.
        const QVariant change = QVariantMap{{"mode", "side_by_side"}};
        QMetaObject::invokeMethod(large, "setView", Q_ARG(QVariant, change));
        QCOMPARE(r.app.model.compareView("cmp").value("mode").toString(), QStringLiteral("side_by_side"));
        QTest::keyClick(r.app.window, Qt::Key_Escape);
        QTRY_VERIFY(!popup->property("visible").toBool());
    }

    // Copy A and Copy B put the full file on the clipboard.
    void copyAAndBPutTheFullFilesOnTheClipboard() {
        QTemporaryDir files;
        ActionApp r(graphOf(write(files.filePath("a.png"), 16, 10, Qt::red), write(files.filePath("b.png"), 16, 10, Qt::blue)));
        QVERIFY(r.app.open());
        r.run();
        r.app.model.select({"cmp"});
        for (const char *which : {"a", "b"}) {
            QVERIFY(r.app.commands.trigger(QStringLiteral("compare.copy-") + QLatin1String(which)));
            QTRY_VERIFY(QGuiApplication::clipboard()->mimeData() && QGuiApplication::clipboard()->mimeData()->hasFormat("image/png"));
            QFile file(r.file(which));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(QGuiApplication::clipboard()->mimeData()->data("image/png"), file.readAll());
            QGuiApplication::clipboard()->clear();
        }
        // The view as drawn: grabbed from the node's viewer, a decoded image on the clipboard.
        QTRY_VERIFY(r.viewer());
        QVERIFY(r.app.commands.trigger("compare.copy-view"));
        QTRY_VERIFY(QGuiApplication::clipboard()->mimeData() && QGuiApplication::clipboard()->mimeData()->hasImage());
        const QImage view = qvariant_cast<QImage>(QGuiApplication::clipboard()->mimeData()->imageData());
        QVERIFY(!view.isNull());
        QVERIFY(view.width() > 0);
    }

    // 100% follows the larger picture (#420): B is 3x A, so the first press is of B, the second of A, the third of B.
    void hundredPercentFollowsTheLargerPicture() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 16, 10, Qt::red), write(files.filePath("b.png"), 48, 30, Qt::blue)));
        QVERIFY(r.app.open());
        r.run();
        r.app.model.select({"cmp"});
        const auto view = [&](const char *key) { return r.app.model.compareView("cmp").value(key).toBool(); };
        QVERIFY(r.app.commands.trigger("compare.actual"));
        QVERIFY(view("actual") && view("actualB"));
        QVERIFY(r.app.commands.trigger("compare.actual"));
        QVERIFY(view("actual") && !view("actualB"));
        QVERIFY(r.app.commands.trigger("compare.actual"));
        QVERIFY(view("actual") && view("actualB"));
        QVERIFY(r.app.commands.trigger("compare.fit"));
        QVERIFY(!view("actual") && !view("actualB"));
        QCOMPARE(r.app.model.undoStack()->count(), 0);
    }

    // Equal sizes: 100% stays A's and never toggles.
    void hundredPercentOfEqualPicturesStaysA() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 16, 10, Qt::red), write(files.filePath("b.png"), 16, 10, Qt::blue)));
        QVERIFY(r.app.open());
        r.run();
        r.app.model.select({"cmp"});
        for (int press = 0; press < 2; ++press) {
            QVERIFY(r.app.commands.trigger("compare.actual"));
            QVERIFY(r.app.model.compareView("cmp").value("actual").toBool());
            QVERIFY(!r.app.model.compareView("cmp").value("actualB").toBool());
        }
    }

    // The keys belong to the node: with no Compare Images selected they are not taken.
    void theKeysApplyOnlyWithACompareNodeSelected() {
        QTemporaryDir files;
        RunApp r(graphOf(write(files.filePath("a.png"), 16, 10, Qt::red), write(files.filePath("b.png"), 16, 10, Qt::blue)));
        QVERIFY(r.app.open());
        r.app.model.select({"left"});
        QVERIFY(!r.app.commands.contextActive("compare"));
        r.app.model.select({"cmp"});
        QVERIFY(r.app.commands.contextActive("compare"));
    }
};

QTEST_MAIN(TestCanvasCompare)
#include "test_canvas_compare.moc"
