// The App view in the real window (#385, Main.qml, offscreen, QTest events only): a workflow with an app block opens as the
// form with the canvas hidden, Run queues the bound copy, a result shows, Open in Graph and Show as app switch the tab, and a
// workflow with no block is the window it was. The store's logic is test_app_view; this is only what QML wiring shows.

#include <QJsonDocument>
#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"
#include "apps/app_window.h"
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

void tap(QQuickWindow *window, QQuickItem *item) {
    QTest::mouseClick(window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
}

// The runs and the view tied to a window as main.cpp ties them; the run is a fake that only records the request (never started).
struct Rig {
    RunSetup setup;
    QList<RunRequest> started;
    RunService service;
    CanvasRuns runs;
    AppView view;

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
               [](const QString &) {}, &app.registry) {
        runs.registerCommands(app.commands);
        app.runs = &runs;
        bindAppView(view, app.workflows, app.model, service, app.commands, runs);
        app.appView = &view;
    }
};

} // namespace

class TestAppWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // A template of the catalog (#387, tests/apps/templates/): opening it by id is the App view; Open in Graph is its workflow as a tab.
    void anAppTemplateOpensAsTheAppAndOpenInGraphIsItsWorkflow() {
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app);
        app.workflows.setTemplateRoots({{QStringLiteral(FUGUE_SOURCE_DIR "/tests/apps/templates"), false}});
        QVERIFY(app.open());
        QVariantMap card;
        for (const QVariant &entry : app.workflows.templates())
            if (entry.toMap().value("id") == "test-app")
                card = entry.toMap();
        QVERIFY(card.value("app").toBool());
        QCOMPARE(card.value("title").toString(), QStringLiteral("Test app"));

        QVERIFY(app.workflows.openTemplateById("test-app"));
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(!find(app.window, "canvas"));
        QCOMPARE(find(app.window, "appTitle")->property("text").toString(), QStringLiteral("Block title")); // the view is the binding's; the card's words are the catalog's
        QVERIFY(find(app.window, "appInput-prompt"));

        rig.view.openInGraph(); // the button is the first test's
        QTRY_VERIFY(find(app.window, "canvas"));
        QCOMPARE(app.model.document().graph.nodes.value("encode").inputs.value("prompt").toJson(), QJsonValue("a quiet lake"));
        qDeleteAll(app.engine.rootObjects());
    }

    // A block that is not well formed, or of a later format, opens as the graph and says why, once, as other load problems are said.
    void anUnusableBlockOpensTheGraphAndSaysWhy() {
        for (const char *block : {"{\"format\": 1, \"inputs\": \"nope\"}", "{\"format\": 99}"}) {
            const QByteArray json = QByteArray(R"({"fugue": 1, "app": @@, "graph": {"nodes": {
                "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "old words"}}}}})")
                                        .replace("@@", block);
            const Result<Document> loaded = Document::load(json);
            QVERIFY2(loaded.ok(), block);
            App app(loaded.value());
            Rig rig(app);
            QVERIFY(app.open());
            QTRY_VERIFY(find(app.window, "canvas"));
            QVERIFY(!find(app.window, "appPage"));
            QVERIFY2(!app.workflows.notice().isEmpty(), block);
            QVERIFY2(app.workflows.notice().contains("opens as a graph"), qPrintable(app.workflows.notice()));
            qDeleteAll(app.engine.rootObjects());
        }
    }

    void anAppOpensAsTheFormRunsAndGoesToTheGraphAndBack() {
        App app(load(appWorkflow));
        Rig rig(app);
        QList<RunRequest> &started = rig.started;
        RunService &service = rig.service;
        QVERIFY(app.open());

        // The form, no canvas.
        QVERIFY(find(app.window, "appPage"));
        QVERIFY(!find(app.window, "canvas"));
        QVERIFY(!find(app.window, "areaHeader-1")); // no graph menus over the form
        QCOMPARE(find(app.window, "appTitle")->property("text").toString(), QStringLiteral("Restyle"));
        QVERIFY(!find(app.window, "appCancel"));
        QVERIFY(!find(app.window, "appShowApp"));

        // Typing in the form is the value a run binds; Run, the key and the button, reaches the app, not the graph.
        QQuickItem *prompt = find(app.window, "appInput-prompt");
        QVERIFY(prompt);
        QCOMPARE(prompt->property("text").toString(), QStringLiteral("old words"));
        prompt->setProperty("text", "a red fox");
        QMetaObject::invokeMethod(prompt, "textEdited");
        QVERIFY(app.commands.run("run.start"));
        QCOMPARE(started.size(), 1);
        QCOMPARE(started.first().document.graph.nodes.value("encode").inputs.value("prompt").toJson(), QJsonValue("a red fox"));
        QTRY_VERIFY(find(app.window, "appCancel"));
        QVERIFY(!find(app.window, "appRun")->property("enabled").toBool());
        const QString picture = app.dir.path() + "/result.png";
        QVERIFY(QImage(8, 8, QImage::Format_RGB32).save(picture));
        emit service.event("r1", {{"event", "node.done"}, {"node", "save"},
                                  {"thumbnails", QJsonObject{{"image", QJsonObject{{"url", QUrl::fromLocalFile(picture).toString()}, {"width", 8}, {"height", 8}}}}}});
        emit service.event("r1", {{"event", "run.done"}, {"status", "ok"}});
        QTRY_VERIFY(!find(app.window, "appCancel"));
        QTRY_COMPARE(find(app.window, "appResults")->property("count").toInt(), 1);
        if (const QByteArray dir = qgetenv("FUGUE_SHOT_DIR"); !dir.isEmpty()) { // FUGUE_SHOT_DIR=<dir>: a picture of the view
            QTest::qWait(300);
            test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/app-view.png");
        }
        tap(app.window, find(app.window, "appRun"));
        QCOMPARE(started.size(), 2);
        emit service.event("r2", {{"event", "run.done"}, {"status", "cancelled"}});

        // Open in Graph: this tab shows the graph, with the way back; Show as app returns.
        tap(app.window, find(app.window, "appOpenInGraph"));
        QTRY_VERIFY(find(app.window, "canvas"));
        QTRY_VERIFY(!find(app.window, "appPage"));
        QTRY_VERIFY(find(app.window, "appShowApp")->mapToScene(QPointF()).y() > find(app.window, "canvas")->mapToScene(QPointF()).y());
        app.commands.run("run.start"); // the graph's own run now
        QCOMPARE(started.size(), 2);
        tap(app.window, find(app.window, "appShowApp"));
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(!find(app.window, "canvas"));
        QTRY_VERIFY(!find(app.window, "appShowApp"));

        // A workflow with no block is the window it was: no page, no way back.
        app.workflows.newWorkflow();
        QTRY_VERIFY(find(app.window, "canvas"));
        QTRY_VERIFY(!find(app.window, "appPage"));
        QTRY_VERIFY(!find(app.window, "appShowApp"));
        qDeleteAll(app.engine.rootObjects());
    }
};

QTEST_MAIN(TestAppWindow)
#include "test_app_window.moc"
