// The App view as an area's view type (#450, spec 12.16 and 12.17) in the real window, offscreen, QTest events only: one area is the
// window it was (the tab mode); in a split an area shows the App beside the graph, toggling (view.toggle-app, the switch, the rail's
// Graph) trades the active area's view, the page is made only while an area shows the App, a workflow with no app block says so in
// the area, and closing the split gives the default window back.

#include <QDir>
#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"
#include "../window_grab.h"
#include "../workspace/window_items.h"
#include "apps/app_shelf.h"
#include "apps/app_window.h"
#include "engine/paths.h"
#include "engine/settings.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

using namespace canvas_test;
using namespace fugue;

namespace {

const char *appWorkflow = R"({
  "fugue": 1,
  "app": {"format": 1, "title": "Restyle", "defaultView": "app",
          "inputs": [{"role": "prompt", "targets": ["encode.prompt"], "label": "Prompt"}],
          "outputs": [{"node": "save", "label": "Picture"}]},
  "graph": {"nodes": {
    "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "old words"}},
    "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out"}}
  }}
})";


const char *zWorkflow = R"JSON({
  "fugue": 1,
  "app": {"format": 1, "title": "Restyle", "defaultView": "app",
          "inputs": [{"role": "prompt", "targets": ["encode.prompt"], "label": "Prompt"}],
          "outputs": [{"node": "save", "label": "Picture"}]},
  "requires": {"models": [{"resource": "fugue/diffusion-model", "hash": "sha256:7777777777777777777777777777777777777777777777777777777777777777", "name": "z_image_turbo_int8_convrot.safetensors", "source": "http:\u002f\u002f127.0.0.1:9/z.safetensors", "size": 5000000000}]},
  "graph": {"nodes": {
    "models": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "sha256:7777777777777777777777777777777777777777777777777777777777777777", "name": "z_image_turbo_int8_convrot.safetensors"}}},
    "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "old words"}},
    "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out"}}
  }}
})JSON";

void writeZ(const QString &folder) {
    const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]}, "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
    QByteArray file;
    for (int i = 0; i < 8; ++i)
        file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
    QDir().mkpath(folder + "/diffusion_models");
    QFile out(folder + "/diffusion_models/z_image_turbo_bf16.safetensors");
    if (out.open(QIODevice::WriteOnly))
        out.write(file + header);
}

const char *blank = R"({"fugue": 1, "graph": {"nodes": {}}})";

QQuickItem *shown(QQuickItem *from, const QString &name) {
    for (QQuickItem *child : from->childItems()) {
        if (!child->isVisible())
            continue;
        if (child->objectName() == name)
            return child;
        if (QQuickItem *found = shown(child, name))
            return found;
    }
    return nullptr;
}
QQuickItem *find(QQuickWindow *window, const QString &name) { return shown(window->contentItem(), name); }

QRectF sceneRect(QQuickItem *item) { return QRectF(item->mapToScene(QPointF(0, 0)), QSizeF(item->width(), item->height())); }
QPoint centerOf(QQuickItem *item) { return sceneRect(item).center().toPoint(); }

// The runs and the view tied to a window as main.cpp ties them; the run is a fake that only records the request.
struct Rig {
    RunSetup setup;
    QList<RunRequest> started;
    RunService service;
    CanvasRuns runs;
    AppView view;
    AppShelf shelf;

    static RunSetup setupIn(const QString &root) {
        RunSetup out;
        out.runsRoot = root + "/runs";
        out.queueDir = root + "/queue";
        out.lockPath = root + "/engine.lock";
        out.cacheRoot = root + "/results";
        out.hashesPath = root + "/hashes.json";
        out.thumbnailDir = root + "/thumbnails";
        out.collectGarbage = false;
        return out;
    }
    explicit Rig(App &app)
        : setup(setupIn(app.dir.path())), service(setup), runs(&app.model, &app.workflows, &service, &app.registry),
          view([this](const RunRequest &request) { started << request; return QStringLiteral("r%1").arg(started.size()); },
               [](const QString &) {}, &app.registry),
          shelf([&app] { return app.workflows.templates(); }, [&app](const QString &id) { return app.workflows.openTemplateById(id); }) {
        app.appShelf = &shelf;
        runs.registerCommands(app.commands);
        app.runs = &runs;
        bindAppView(view, app.workflows, app.model, service, app.commands, runs);
        app.appView = &view;
    }
};

} // namespace

class TestAppAreaWindow : public QObject {
    Q_OBJECT

    // The area tree after a split, with the App in area 2.
    static bool splitWithApp(App &app) {
        return app.commands.trigger("area.split-row") && app.workspace.showView(2, "app");
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // #616: the Get ready row's "Use what you have" in the App view: the file is read, the app keeps it, the row leaves the sheet.
    void theGetReadyRowUsesAFileInTheAppViewAndLeavesThePlan() {
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        writeZ(models.path());
        App app(load(zWorkflow));
        Rig rig(app);
        QVERIFY(app.open());
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(rig.view.shown());
        ReadinessSheet *sheet = app.workflows.readiness();
        sheet->show();
        QCOMPARE(sheet->view().value("models").toList().size(), 1);
        const QString hash = sheet->view().value("models").toList().at(0).toMap().value("hash").toString();
        sheet->useOnDisk(hash);
        QVERIFY(app.workflows.hashingUses());
        QTRY_VERIFY(!app.workflows.hashingUses());
        QCOMPARE(rig.view.usesList().size(), 1); // the app's own state: its workflow is not written
        QVERIFY(sheet->view().value("models").toList().isEmpty());
        qDeleteAll(app.engine.rootObjects());
    }

    void oneAreaIsTheTabModeAsBefore() {
        App app(load(appWorkflow));
        Rig rig(app);
        QVERIFY(app.open());
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(!find(app.window, "appArea")); // no area made it: the window's one area is the tab mode
        QVERIFY(rig.view.shown());
        QVERIFY(find(app.window, "appViewSwitch"));
        QVERIFY(app.warnings.isEmpty());
        qDeleteAll(app.engine.rootObjects());
    }

    void anAppTabInASplitShowsTheAppInOneAreaAndTheGraphInTheOther() {
        App app(load(appWorkflow));
        Rig rig(app);
        QVERIFY(app.open());
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(app.commands.trigger("area.split-row"));
        QTRY_VERIFY(find(app.window, "areaHost"));
        // The new area is a Canvas: the tab's page is gone with the one-area mode, and nothing shows the App.
        QTRY_VERIFY(!find(app.window, "appPage"));
        QVERIFY(!rig.view.shown());
        QVERIFY(app.workspace.showView(2, "app"));
        QTRY_VERIFY(find(app.window, "appArea") && find(app.window, "appPage"));
        QVERIFY(find(app.window, "canvas")); // the graph in the other area
        QVERIFY(rig.view.shown());           // the active area (2) shows the App
        QTest::qWait(80);
        QVERIFY(sceneRect(find(app.window, "appArea")).left() >= sceneRect(find(app.window, "canvas")).right() - 1);
        QCOMPARE(find(app.window, "appTitle")->property("title").toString(), QStringLiteral("Restyle"));
        QVERIFY(find(app.window, "appViewSwitch"));

        // A press in the graph's area makes it active: the App view is not what the keys and the run act on there.
        app.workspace.setActive(1);
        QTRY_VERIFY(!rig.view.shown());
        app.workspace.setActive(2);
        QTRY_VERIFY(rig.view.shown());

        // Toggling in the App's area gives it the Graph (the two trade); again, and it is the App's.
        QVERIFY(app.commands.trigger("view.toggle-app"));
        QTRY_COMPARE(app.workspace.viewOf(2), QString("graph"));
        QCOMPARE(app.workspace.viewOf(1), QString("app"));
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(!rig.view.shown()); // the active area 2 shows the graph now
        QVERIFY(app.commands.trigger("view.toggle-app")); // from the graph's area: the App comes here
        QTRY_COMPARE(app.workspace.viewOf(2), QString("app"));
        QCOMPARE(app.workspace.viewOf(1), QString("graph"));

        // The App|Graph switch is the same command.
        tapSwitch(app, "appViewSwitchGraph");
        QTRY_COMPARE(app.workspace.viewOf(2), QString("graph"));
        tapSwitch(app, "appViewSwitchApp");
        QTRY_COMPARE(app.workspace.viewOf(2), QString("app"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
        qDeleteAll(app.engine.rootObjects());
    }

    void thePageIsMadeOnlyWhileAnAreaShowsTheApp() {
        App app(load(appWorkflow));
        Rig rig(app);
        QVERIFY(app.open());
        QVERIFY(splitWithApp(app));
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(app.workspace.showView(2, "canvas"));
        QTRY_VERIFY(find(app.window, "canvasArea"));
        QTRY_VERIFY(!find(app.window, "appPage"));
        QVERIFY(!find(app.window, "appArea"));
        // From the graph's area, toggling finds the other area for the App.
        app.workspace.setActive(1);
        QVERIFY(app.commands.trigger("view.toggle-app"));
        QTRY_COMPARE(app.workspace.viewOf(2), QString("app"));
        QTRY_VERIFY(find(app.window, "appPage"));
        // The rail's Graph item is the same toggle, from the App's area.
        app.workspace.setActive(2);
        QTest::qWait(100);
        QQuickItem *rail = find(app.window, "appOpenInGraph");
        QVERIFY(rail);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(rail));
        QTRY_COMPARE(app.workspace.viewOf(2), QString("graph"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
        qDeleteAll(app.engine.rootObjects());
    }

    void aTabWithNoAppBlockSaysSoInTheAreaAndTheAppComesBackWithItsTab() {
        App app(load(appWorkflow));
        Rig rig(app);
        QVERIFY(app.open());
        QVERIFY(splitWithApp(app));
        QTRY_VERIFY(find(app.window, "appPage"));
        app.workflows.newWorkflow();
        QTRY_VERIFY(find(app.window, "appAreaNote"));
        QTRY_VERIFY(!find(app.window, "appPage"));
        QVERIFY(!rig.view.shown());
        QVERIFY(find(app.window, "appViewSwitch")); // always shown (App | Graph, owner 2026-11-25)
        app.workflows.setCurrent(0);
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(!find(app.window, "appAreaNote"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
        qDeleteAll(app.engine.rootObjects());
    }

    void closingTheSplitGivesTheDefaultWindowBack() {
        App app(load(blank));
        Rig rig(app);
        QVERIFY(app.open());
        QTest::qWait(200);
        const QString before = window_items::describe(app.window->contentItem());
        QVERIFY(splitWithApp(app));
        QTRY_VERIFY(find(app.window, "appAreaNote")); // not an app
        QVERIFY(app.commands.trigger("area.close"));
        QTRY_VERIFY(!find(app.window, "areaHost"));
        QTest::qWait(200);
        QCOMPARE(window_items::describe(app.window->contentItem()), before);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
        qDeleteAll(app.engine.rootObjects());
    }

    void closingTheSplitOfAnAppTabIsTheAppViewAgain() {
        App app(load(appWorkflow));
        Rig rig(app);
        QVERIFY(app.open());
        QVERIFY(splitWithApp(app));
        QTRY_VERIFY(find(app.window, "appArea"));
        QVERIFY(app.commands.trigger("area.close"));
        QTRY_VERIFY(!find(app.window, "areaHost"));
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(!find(app.window, "appArea"));
        QVERIFY(rig.view.shown());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
        qDeleteAll(app.engine.rootObjects());
    }

    // FUGUE_APP_AREA_SHOTS=<dir> [FUGUE_SHOT_THEME=<omarchy theme>] [FUGUE_SHOT_LOOK="preset=flat"]: App at the start, Graph at the end.
    void screenshots() {
        const QString out = qEnvironmentVariable("FUGUE_APP_AREA_SHOTS");
        if (out.isEmpty())
            QSKIP("Set FUGUE_APP_AREA_SHOTS to a folder to write the pictures there.");
        QDir().mkpath(out);
        const QString tag = qEnvironmentVariable("FUGUE_SHOT_TAG", QStringLiteral("shot"));
        App app(load(appWorkflow), {}, Look{qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("tokyo-night")), 12});
        Rig rig(app);
        QVERIFY(app.open());
        QCOMPARE(app.settings.applyLookPreset(qEnvironmentVariable("FUGUE_SHOT_PRESET", QStringLiteral("calm"))), QString());
        QVERIFY(app.commands.trigger("area.split-row"));
        QVERIFY(app.workspace.showView(2, "graph")); // the Graph at the end, the App takes its old area at the start
        QVERIFY(app.workspace.showView(1, "app"));
        app.workspace.setActive(1);
        QTRY_VERIFY(find(app.window, "appPage"));
        for (const QSize size : {QSize(1888, 971), QSize(1100, 700)}) {
            app.window->resize(size);
            QTest::mouseMove(app.window, QPoint(size.width() / 2, size.height() / 2));
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/%2-%3x%4.png").arg(out, tag).arg(size.width()).arg(size.height())));
        }
    }

private:
    static void tapSwitch(App &app, const char *name) {
        QQuickItem *item = find(app.window, name);
        QVERIFY(item);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(item));
    }
};

QTEST_MAIN(TestAppAreaWindow)
#include "test_app_area_window.moc"
