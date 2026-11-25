// The apps shelf in the real window (#388, Main.qml, offscreen, QTest events only): a window with nothing open shows the shelf in
// the canvas's place, a ready app opens in the App view, a not-ready one opens the one Get ready sheet over its App view, Start
// blank gives the graph, File > Apps… is the same shelf over open tabs, and a window with a file open has no shelf at all. The
// cards' logic is test_app_shelf; this is only what QML wiring shows.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"
#include "apps/app_shelf.h"
#include "apps/app_window.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

using namespace canvas_test;
using namespace fugue;

namespace {

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

// Taps the item of this name once it is where it will stay: a control made just now is placed over the next frames (the grid
// learns its width). Found again each time, as the window may make it again meanwhile.
void tap(QQuickWindow *window, const QString &name) {
    QRectF last;
    int still = 0;
    QTRY_VERIFY2(([&] {
        QQuickItem *item = find(window, name);
        if (!item)
            return false;
        const QRectF now = item->mapRectToScene(QRectF(QPointF(), item->size()));
        still = now == last ? still + 1 : 0;
        last = now;
        return still >= 3;
    }()), "the item is placed");
    QQuickItem *item = find(window, name);
    const QPoint at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    QTest::mouseMove(window, at); // the card is hovered before it is pressed, as a person's pointer does
    QTest::mouseClick(window, Qt::LeftButton, {}, at);
}

void write(const QString &path, const QJsonObject &json) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(QJsonDocument(json).toJson());
}

// An app template; `needsModel` names one model, with a source to get it from, that is not on this machine.
void makeApp(const QString &root, const QString &id, const QString &title, bool needsModel) {
    QJsonObject nodes{{"encode", QJsonObject{{"type", "fugue/text-encode"}, {"version", "1.0.0"}, {"inputs", QJsonObject{{"prompt", "a lake"}}}}},
                      {"save", QJsonObject{{"type", "fugue/save-image"}, {"version", "1.1.0"}, {"inputs", QJsonObject{{"name", "out"}}}}}};
    QJsonObject flow{{"fugue", 1},
                     {"app", QJsonObject{{"format", 1}, {"title", title}, {"defaultView", "app"},
                                         {"inputs", QJsonArray{QJsonObject{{"role", "prompt"}, {"targets", QJsonArray{"encode.prompt"}}, {"label", "Prompt"}}}},
                                         {"outputs", QJsonArray{QJsonObject{{"node", "save"}}}}}},
                     {"graph", QJsonObject{{"nodes", nodes}}}};
    if (needsModel) {
        const QJsonObject model{{"resource", "fugue/diffusion-model"}, {"hash", "sha256:" + QString(64, 'a')}, {"name", "needed.gguf"}};
        QJsonObject recorded = model;
        recorded.insert("source", "http://127.0.0.1:9/needed.gguf"); // never fetched: the test does not say yes
        recorded.insert("size", 1048576);
        nodes.insert("models", QJsonObject{{"type", "fugue/load-models"}, {"version", "1.0.0"}, {"inputs", QJsonObject{{"diffusion_model", model}}}});
        flow.insert("graph", QJsonObject{{"nodes", nodes}});
        flow.insert("requires", QJsonObject{{"models", QJsonArray{recorded}}});
    }
    write(root + "/" + id + "/workflow.fugue", flow);
    write(root + "/" + id + "/template.json",
          QJsonObject{{"title", title}, {"description", "Makes " + title}, {"app", QJsonObject{{"title", title}, {"summary", "Does " + title}}}});
}

// What main.cpp makes: the App view (nothing runs here) and the shelf over the window's templates and its open.
struct Rig {
    RunSetup setup;
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
          view([](const RunRequest &) { return QString(); }, [](const QString &) {}, &app.registry),
          shelf([&app] { return app.workflows.templates(); }, [&app](const QString &id) { return app.workflows.openTemplateById(id); }) {
        runs.registerCommands(app.commands);
        app.runs = &runs;
        bindAppView(view, app.workflows, app.model, service, app.commands, runs);
        app.appView = &view;
        QObject::connect(&app.workflows, &Workflows::tabsChanged, &shelf, [this, &app] { shelf.setLanding(app.workflows.nothingOpen()); });
        app.appShelf = &shelf;
    }
};

} // namespace

class TestAppShelfWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // Nothing open: the shelf lists the apps (not the plain templates), ready first. A ready one opens in the App view; the
    // shelf goes. A not-ready one opens its App view and the one Get ready sheet over it.
    void theShelfOpensAReadyAppAndAsksOnceForANotReadyOne() {
        QTemporaryDir root;
        makeApp(root.path(), "ready-app", "Ready app", false);
        makeApp(root.path(), "needs-app", "Needs app", true);
        write(root.path() + "/plain/workflow.fugue", QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", QJsonObject()}}}});
        write(root.path() + "/plain/template.json", QJsonObject{{"title", "Plain"}});
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app);
        app.workflows.setTemplateRoots({{root.path(), false}});
        QVERIFY(app.open());
        rig.shelf.setLanding(app.workflows.nothingOpen()); // main.cpp does it once the files are in
        QTRY_VERIFY(find(app.window, "appsHome"));
        QVERIFY(!find(app.window, "canvas"));
        QTRY_VERIFY(find(app.window, "appCard-ready-app"));
        QTRY_VERIFY(find(app.window, "appCard-needs-app"));
        QVERIFY(!find(app.window, "appCard-plain"));
        QCOMPARE(find(app.window, "appReady-ready-app")->property("text").toString(), QStringLiteral("Ready"));
        QVERIFY(find(app.window, "appReady-needs-app")->property("text").toString().startsWith("Needs"));
        const QPointF readyAt = find(app.window, "appCard-ready-app")->mapToScene(QPointF());
        const QPointF needsAt = find(app.window, "appCard-needs-app")->mapToScene(QPointF());
        QVERIFY(readyAt.y() < needsAt.y() || (readyAt.y() == needsAt.y() && readyAt.x() < needsAt.x()));

        tap(app.window, "appCard-needs-app");
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(!find(app.window, "appsHome"));
        QVERIFY(app.workflows.readiness()->isOpen()); // the one sheet, over the app's own screen
        QCOMPARE(app.workflows.count(), 1);
        app.workflows.readiness()->cancel();
        QTRY_VERIFY(!app.workflows.readiness()->isOpen());
        qDeleteAll(app.engine.rootObjects());
    }

    void aReadyAppOpensInTheAppViewWithNoSheet() {
        QTemporaryDir root;
        makeApp(root.path(), "ready-app", "Ready app", false);
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app);
        app.workflows.setTemplateRoots({{root.path(), false}});
        QVERIFY(app.open());
        rig.shelf.setLanding(app.workflows.nothingOpen());
        QTRY_VERIFY(find(app.window, "appCard-ready-app"));
        tap(app.window, "appCard-ready-app");
        QTRY_VERIFY(find(app.window, "appPage"));
        QTRY_VERIFY(!find(app.window, "appsHome"));
        QCOMPARE(find(app.window, "appTitle")->property("text").toString(), QStringLiteral("Ready app"));
        QVERIFY(!app.workflows.readiness()->isOpen());
        qDeleteAll(app.engine.rootObjects());
    }

    // Start blank is the graph, and it stays so for the session; File > Apps… is the same shelf over what is open.
    void startBlankGivesTheGraphAndFileAppsIsTheSameShelf() {
        QTemporaryDir root;
        makeApp(root.path(), "ready-app", "Ready app", false);
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app);
        app.workflows.setTemplateRoots({{root.path(), false}});
        QVERIFY(app.open());
        rig.shelf.setLanding(app.workflows.nothingOpen());
        QTRY_VERIFY(find(app.window, "appsStartBlank"));
        tap(app.window, "appsStartBlank");
        QTRY_VERIFY(!find(app.window, "appsHome"));
        QTRY_VERIFY(find(app.window, "canvas"));

        QVERIFY(app.commands.run("file.apps"));
        QTRY_VERIFY(find(app.window, "appCard-ready-app"));
        QVERIFY(app.window->findChild<QObject *>("appsDialog")->property("visible").toBool());
        tap(app.window, "appCard-ready-app");
        QTRY_VERIFY(find(app.window, "appPage"));
        QTRY_VERIFY(!app.window->findChild<QObject *>("appsDialog")->property("visible").toBool());
        qDeleteAll(app.engine.rootObjects());
    }

    // A window with a workflow already open never has the shelf: no item, and the catalog is never read.
    void aWindowWithSomethingOpenHasNoShelfAndReadsNothing() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {"a": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {}}}}})"));
        int reads = 0;
        AppShelf shelf([&] { ++reads; return QVariantList(); }, [](const QString &) { return false; });
        QObject::connect(&app.workflows, &Workflows::tabsChanged, &shelf, [&] { shelf.setLanding(app.workflows.nothingOpen()); });
        app.appShelf = &shelf;
        QVERIFY(app.open());
        shelf.setLanding(app.workflows.nothingOpen());
        QVERIFY(!shelf.atHome());
        QVERIFY(find(app.window, "canvas"));
        QVERIFY(!find(app.window, "appsHome"));
        QCOMPARE(reads, 0);
        qDeleteAll(app.engine.rootObjects());
    }
};

QTEST_MAIN(TestAppShelfWindow)
#include "test_app_shelf_window.moc"
