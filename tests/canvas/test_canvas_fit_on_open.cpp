// Opening a workflow in the real window (Main.qml, offscreen, #162): one with no saved view is
// fitted to its nodes as View > Fit does, clear of the run toolbar that floats over the canvas, the
// first template in a fresh window as much as one in a later tab; one with a saved view keeps it.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>
#include <QtTest>

#include "canvas_app.h"
#include "engine/pack_loader.h"
#include "engine/templates.h"
#include "run/canvas_runs.h"
#include "run/run_bar.h"
#include "run/run_queue.h"
#include "run/run_service.h"

using namespace canvas_test;
using namespace fugue;

namespace {

const char *empty = R"({"fugue": 1, "graph": {"nodes": {}}})";

// The window with its run toolbar, as test_canvas_run makes it, but with no provider.
struct BarApp {
    App app;
    std::unique_ptr<RunService> service;
    std::unique_ptr<CanvasRuns> runs;
    std::unique_ptr<RunBar> bar;
    std::unique_ptr<RunQueue> queue;

    BarApp() : app(load(empty)) {
        RunSetup setup;
        setup.addNodeTypes = [](Registry &registry) {
            nodes::addBuiltinNodeTypes(registry);
            loadInstalledPacks(registry);
        };
        setup.addKernels = nodes::addBuiltinKernels;
        setup.registerProviders = [](Registry &) { return Errors(); };
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
        app.runs = runs.get();
        bar = std::make_unique<RunBar>(runs.get(), root + "/run-bar.json", &app.chrome);
        queue = std::make_unique<RunQueue>(RunQueuePaths{root + "/runs", root + "/queue", root + "/engine.lock"}, service.get());
        app.runBar = bar.get();
        app.runQueue = queue.get();
    }
    ~BarApp() {
        qDeleteAll(app.engine.rootObjects()); // the window goes before the runs it shows
        queue.reset();
        bar.reset();
        runs.reset();
        service.reset();
    }
};

// A bundled template, as the Templates dialog hands it to the window.
Template bundled(const QString &id) {
    const QString folder = QStringLiteral(FUGUE_SOURCE_DIR "/templates/") + id;
    Template chosen;
    chosen.id = id;
    chosen.title = id;
    chosen.folder = folder;
    chosen.workflow = folder + "/workflow.fugue";
    return chosen;
}

} // namespace

class TestFitOnOpen : public QObject {
    Q_OBJECT

    // Every node inside the canvas and clear of the run toolbar's box (canvas coordinates).
    // The fit comes once the opener is done (the next turn of the event loop).
    static void settle() { QTest::qWait(20); }

    static void verifyFits(BarApp &r, const QString &what) {
        App &app = r.app;
        QQuickItem *bar = item(app.canvas, "runToolbar");
        QVERIFY2(bar, "the run toolbar is shown");
        auto *surface = bar->property("surface").value<QQuickItem *>();
        QVERIFY(surface && surface->height() > 0);
        const QRectF barBox(app.canvas->mapFromItem(surface, QPointF(0, 0)), QSizeF(surface->width(), surface->height()));
        const NodeList *nodes = app.model.nodes();
        QVERIFY(nodes->rowCount() > 1);
        const QRectF area(0, 0, app.canvas->width(), app.canvas->height());
        const qreal z = app.zoom();
        for (int i = 0; i < nodes->rowCount(); ++i) {
            const QModelIndex row = nodes->index(i);
            const QRectF box(app.world->x() + nodes->data(row, NodeList::X).toReal() * z,
                             app.world->y() + nodes->data(row, NodeList::Y).toReal() * z,
                             nodes->data(row, NodeList::Width).toReal() * z, nodes->data(row, NodeList::Height).toReal() * z);
            const QString name = what + ": " + nodes->data(row, NodeList::NodeId).toString();
            QVERIFY2(area.contains(box), qPrintable(name + " is inside the canvas"));
            QVERIFY2(!box.intersects(barBox), qPrintable(name + " is clear of the run toolbar"));
        }
        QVERIFY(z < 1);
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void aTemplateFitsInAFreshWindow() {
        BarApp r;
        QVERIFY(r.app.open());
        QVERIFY(r.app.workflows.openTemplate(bundled("z-image-turbo")));
        settle();
        QCOMPARE(r.app.workflows.count(), 1); // it took the empty tab
        verifyFits(r, "z-image-turbo");
    }

    void aTemplateInALaterTabFitsToo() {
        BarApp r;
        QVERIFY(r.app.open());
        QVERIFY(r.app.workflows.openTemplate(bundled("flux2-klein-4b")));
        QVERIFY(r.app.workflows.openTemplate(bundled("z-image-turbo")));
        settle();
        QCOMPARE(r.app.workflows.count(), 2);
        verifyFits(r, "second tab");
        // Back and forth: each tab keeps the view it was fitted to.
        const qreal zoom = r.app.zoom();
        r.app.workflows.setCurrent(0);
        settle();
        verifyFits(r, "first tab");
        r.app.workflows.setCurrent(1);
        QVERIFY(qAbs(r.app.zoom() - zoom) < 0.001); // a tab's view is kept to three decimals
        verifyFits(r, "second tab again");
    }

    void viewFitGivesTheSameView() {
        BarApp r;
        QVERIFY(r.app.open());
        QVERIFY(r.app.workflows.openTemplate(bundled("z-image-turbo")));
        settle();
        const qreal zoom = r.app.zoom();
        const QPointF pan = r.app.pan();
        r.app.commands.trigger("view.fit");
        QCOMPARE(r.app.zoom(), zoom);
        QVERIFY(qAbs(r.app.pan().x() - pan.x()) < 0.5 && qAbs(r.app.pan().y() - pan.y()) < 0.5);
    }

    void aSavedViewIsKept() {
        BarApp r;
        QVERIFY(r.app.open());
        const QByteArray json = R"({"fugue": 1,
          "graph": {"nodes": {"a": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "x"}},
                              "b": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "y"}}}},
          "layout": {"nodes": {"a": {"pos": [0, 0]}, "b": {"pos": [3000, 2000]}},
                     "view": {"center": [40, 50], "zoom": 1.5}}})";
        QVERIFY(r.app.workflows.openText(QString::fromUtf8(json)));
        settle();
        QCOMPARE(r.app.zoom(), 1.5);
        const QVariantList center = r.app.model.view().value("center").toList();
        QCOMPARE(center.value(0).toDouble(), 40.0);
        QCOMPARE(center.value(1).toDouble(), 50.0);
        // The world point at the canvas's middle is the saved center.
        QCOMPARE((r.app.canvas->width() / 2 - r.app.pan().x()) / r.app.zoom(), 40.0);
    }
};

QTEST_MAIN(TestFitOnOpen)
#include "test_canvas_fit_on_open.moc"
