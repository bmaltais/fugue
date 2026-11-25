// One app, start to finish in the real window (#396, offscreen): a template with an app block opens as the App view, a value is
// set in its form, Run really runs the bound workflow on the CPU (the built-in kernels, no provider, no GPU), the picture shows
// as a result with its take, and Open in Graph shows the graph, which the form's value did not change. The pieces have their own tests
// (test_app_view the store, test_app_window the wiring with a fake run); this is the one that joins them with a real run.

#include <QImage>
#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"
#include "apps/app_window.h"
#include "engine/run_folder.h"
#include "nodes/builtin.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

using namespace canvas_test;
using namespace fugue;

namespace {

// Load Image, Upscale Image by a factor (CPU), Save. The app asks for the picture and the factor.
const char *workflowText = R"({
  "fugue": 1,
  "app": {"format": 1, "title": "Bigger", "defaultView": "app",
          "inputs": [{"role": "image", "targets": ["load.file"], "label": "Picture"},
                     {"name": "factor", "targets": ["up.scale"], "label": "Times bigger", "min": 1, "max": 4}],
          "outputs": [{"node": "save", "label": "Bigger picture"}]},
  "graph": {"nodes": {
    "load": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "inputs/sample.png"}},
    "up": {"type": "fugue/upscale-image", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}, "method": "lanczos", "scale": 2}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "up.image"}, "name": "bigger"}}
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

class TestAppEndToEnd : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void aTemplateAppRunsFromItsFormAndOpensInTheGraph() {
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        const QString root = app.dir.path();

        // A template folder of the test's own: the workflow, its card and a 4 x 3 sample picture.
        const QString folder = root + "/templates/bigger";
        QVERIFY(QDir().mkpath(folder + "/inputs"));
        QVERIFY(QImage(4, 3, QImage::Format_RGB32).save(folder + "/inputs/sample.png"));
        for (const auto &[name, bytes] : {std::pair{"workflow.fugue", QByteArray(workflowText)},
                                          std::pair{"template.json", QByteArray(R"({"title": "Bigger", "description": "Makes a picture bigger.", "category": "image"})")}}) {
            QFile file(folder + "/" + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(bytes);
        }
        app.workflows.setTemplateRoots({{root + "/templates", false}});

        RunSetup setup;
        setup.addNodeTypes = fugue::nodes::addBuiltinNodeTypes;
        setup.addKernels = fugue::nodes::addBuiltinKernels;
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
        QList<RunRequest> started; // what the form bound, kept to look at; the run itself is the real one
        AppView view([&service, &started](const RunRequest &request) { started << request; return service.start(request); },
                     [&service](const QString &run) { service.cancel(run); }, &app.registry);
        bindAppView(view, app.workflows, app.model, service, app.commands, runs);
        app.appView = &view;
        QVERIFY(app.open());

        // The template opens as the form: the picture (its own sample) and the factor, no graph.
        QVERIFY(app.workflows.openTemplateById("bigger"));
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(!find(app.window, "canvas"));
        QTRY_VERIFY(find(app.window, "appImageShown-image"));
        QQuickItem *factor = find(app.window, "appInput-factor");
        QVERIFY(factor);
        factor->forceActiveFocus(); // a slider from 1 to 4 in whole steps, from the workflow's 2: one Right is 3
        QTRY_VERIFY(factor->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Right);
        QTRY_COMPARE(view.valueOf("factor").toDouble(), 3.0);

        // Run: a real run on the CPU; 4 x 3 times 3 is 12 x 9, shown with its take.
        tap(app.window, find(app.window, "appRun"));
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "appResultViewer"), 20000);
        QTRY_COMPARE(find(app.window, "appTakes")->property("count").toInt(), 1);
        QTRY_VERIFY(find(app.window, "appSizeLine")->property("text").toString().startsWith(QStringLiteral("12 × 9 · ")));
        QTRY_VERIFY(!find(app.window, "appCancel"));

        QCOMPARE(started.size(), 1);
        QCOMPARE(started.first().document.graph.nodes.value("up").inputs.value("scale").toJson().toDouble(), 3.0); // the run's copy

        // Open in Graph: the same workflow as a graph. The form's value belongs to the run's copy (spec 4.9): the file keeps its own 2.
        tap(app.window, find(app.window, "appOpenInGraph"));
        QTRY_VERIFY(find(app.window, "canvas"));
        QTRY_VERIFY(!find(app.window, "appPage"));
        QCOMPARE(app.model.document().graph.nodes.value("up").inputs.value("scale").toJson().toDouble(), 2.0);

        qDeleteAll(app.engine.rootObjects());
    }
};

QTEST_MAIN(TestAppEndToEnd)
#include "test_app_end_to_end.moc"
