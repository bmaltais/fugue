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

} // namespace

class TestAppWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void anAppOpensAsTheFormRunsAndGoesToTheGraphAndBack() {
        App app(load(appWorkflow));
        RunSetup setup; // never started: the run is the fake below
        const QString root = app.dir.path();
        setup.runsRoot = root + "/runs";
        setup.queueDir = root + "/queue";
        setup.lockPath = root + "/engine.lock";
        setup.cacheRoot = root + "/results";
        setup.hashesPath = root + "/hashes.json";
        setup.thumbnailDir = root + "/thumbnails";
        setup.collectGarbage = false;
        RunService service(setup);
        CanvasRuns runs(&app.model, &app.workflows, &service, &app.registry);
        runs.registerCommands(app.commands);
        app.runs = &runs;
        QList<RunRequest> started;
        AppView view([&started](const RunRequest &request) { started << request; return QStringLiteral("r%1").arg(started.size()); },
                     [](const QString &) {}, &app.registry);
        bindAppView(view, app.workflows, app.model, service, app.commands, runs);
        app.appView = &view;
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
        const QString picture = root + "/result.png";
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
