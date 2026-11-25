// The App view in the real window (#385, Main.qml, offscreen, QTest events only): a workflow with an app block opens as the
// form with the canvas hidden, Run queues the bound copy, a result shows, Open in Graph and Show as app switch the tab, and a
// workflow with no block is the window it was. The store's logic is test_app_view; this is only what QML wiring shows.

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QImage>
#include <QTemporaryDir>
#include <QMimeData>
#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"
#include "apps/app_shelf.h"
#include "apps/app_takes.h"
#include "apps/app_window.h"
#include "canvas/mask_layer.h"
#include "engine/run_folder.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"
#include "../test_shard.h"
#include "../window_grab.h"

using namespace canvas_test;
using namespace fugue;

namespace {

const char *appWorkflow = R"({
  "fugue": 1,
  "app": {"format": 1, "title": "Restyle", "defaultView": "app",
          "inputs": [{"role": "prompt", "targets": ["encode.prompt"], "label": "Prompt"},
                     {"name": "title", "targets": ["save.name"], "label": "Name"}],
          "outputs": [{"node": "save", "label": "Picture"}]},
  "graph": {"nodes": {
    "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "old words"}},
    "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out"}}
  }}
})";

const char *mediaApp = R"({
  "fugue": 1,
  "app": {"format": 1, "title": "Media", "defaultView": "app",
          "inputs": [{"role": "image", "targets": ["load.file"], "label": "Picture"},
                     {"role": "mask", "targets": ["load.mask"]},
                     {"name": "tapered", "targets": ["grow.tapered_corners"], "label": "Tapered"},
                     {"role": "model", "targets": ["up.model"]},
                     {"role": "seed", "targets": ["sample.seed"]}],
          "outputs": [{"node": "save"}]},
  "graph": {"nodes": {
    "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {}},
    "grow": {"type": "fugue/grow-mask", "version": "1.0.1", "inputs": {"tapered_corners": true}},
    "up": {"type": "fugue/load-upscale-model", "version": "1.0.0", "inputs": {}},
    "sample": {"type": "fugue/sampler", "version": "1.2.0", "inputs": {"seed": 3}},
    "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out"}}
  }}
})";


// An app that looks like a real one: a picture, a prompt, a ranged number, a seed and an Advanced toggle.
const char *lookApp = R"({
  "fugue": 1,
  "app": {"format": 1, "title": "Restore a photo", "defaultView": "app",
          "description": "Cleans up a worn photograph, makes it bigger and brings the colors back, keeping the faces as they are.",
          "inputs": [{"role": "image", "targets": ["load.file"], "label": "Picture"},
                     {"role": "prompt", "targets": ["encode.prompt"], "label": "What is in the picture"},
                     {"name": "steps", "targets": ["sample.steps"], "label": "Make it this many times bigger", "min": 1, "max": 4},
                     {"role": "seed", "targets": ["sample.seed"], "label": "Seed"},
                     {"name": "tapered", "targets": ["grow.tapered_corners"], "label": "Soft corners", "section": "Advanced"}],
          "outputs": [{"node": "save", "label": "Picture"}]},
  "graph": {"nodes": {
    "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "inputs/sample.png"}},
    "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "an old family portrait"}},
    "grow": {"type": "fugue/grow-mask", "version": "1.0.1", "inputs": {"tapered_corners": true}},
    "sample": {"type": "fugue/sampler", "version": "1.2.0", "inputs": {"seed": 3, "steps": 3}},
    "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out"}}
  }}
})";

// A made-up readiness plan (what the Get ready sheet would list) for the Needs panel: two models, one of them held by node "up" of
// mediaWithModel, and no pack, so Download all starts at once.
const QString upscaleHash = "sha256:" + QString(64, QLatin1Char('c'));
const QString otherHash = "sha256:" + QString(64, QLatin1Char('d'));
ReadinessPlan needsPlan() {
    ReadinessPlan plan;
    ReadinessModel up;
    up.name = "x.pth";
    up.resource = "fugue/upscale-model";
    up.license = "MIT";
    up.host = "huggingface.co";
    up.size = 67'000'000;
    up.entry = QJsonObject{{"resource", up.resource}, {"hash", upscaleHash}, {"name", up.name}};
    ReadinessModel big;
    big.name = "flux-2-klein-9b-Q4_K_M.gguf";
    big.resource = "fugue/diffusion-model";
    big.license = "FLUX Non-Commercial";
    big.host = "huggingface.co";
    big.size = 5'900'000'000;
    big.entry = QJsonObject{{"resource", big.resource}, {"hash", otherHash}, {"name", big.name}};
    plan.models << up << big;
    plan.downloadBytes = up.size + big.size;
    return plan;
}

QByteArray mediaWithModel() {
    QByteArray text(mediaApp);
    text.replace(R"("up": {"type": "fugue/load-upscale-model", "version": "1.0.0", "inputs": {}})",
                 QByteArray(R"("up": {"type": "fugue/load-upscale-model", "version": "1.0.0", "inputs": {"model": {"resource": "fugue/upscale-model", "hash": ")")
                     + upscaleHash.toUtf8() + R"(", "name": "x.pth"}}})");
    return text;
}

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
        QCOMPARE(find(app.window, "appTitle")->property("title").toString(), QStringLiteral("Block title")); // the view is the binding's; the card's words are the catalog's
        QVERIFY(find(app.window, "appInput-prompt"));

        rig.view.openInGraph(); // the button is the first test's
        QTRY_VERIFY(find(app.window, "canvas"));
        QCOMPARE(app.model.document().graph.nodes.value("encode").inputs.value("prompt").toJson(), QJsonValue("a quiet lake"));
        qDeleteAll(app.engine.rootObjects());
    }

    // A shipped template's own sample shows in the picture well (the dropdown names it, the well draws it). Opening a template takes
    // the sample into the inputs folder (#376), so the well's file is that copy; it must load.
    void aShippedTemplateShowsItsSampleInThePictureWell() {
        const QString root = QStringLiteral(FUGUE_SOURCE_DIR "/templates");
        if (!QFileInfo::exists(root + "/image-upscale/workflow.fugue"))
            QSKIP("the templates submodule is not checked out");
        for (const auto &[id, stem] : {std::pair{"image-upscale", "ramen"}, std::pair{"flux2-klein-9b-da-restore", "reading-nook"}}) {
            App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
            Rig rig(app);
            app.workflows.setTemplateRoots({{root, false}});
            QVERIFY(app.open());
            QVERIFY(app.workflows.openTemplateById(id));
            QTRY_VERIFY(find(app.window, "appPage"));
            QQuickItem *picture = nullptr;
            QTRY_VERIFY((picture = find(app.window, "appImageShown-image")) != nullptr);
            QTRY_COMPARE(picture->property("status").toInt(), 1 /* Image.Ready */);
            QVERIFY2(picture->property("source").toUrl().toLocalFile().contains(QString("/%1-").arg(stem)),
                     qPrintable(picture->property("source").toString()));
            qDeleteAll(app.engine.rootObjects());
        }
    }

    // A click on the picture well only focuses it (Ctrl+V then pastes there); only the Browse button opens the file picker.
    void clickingThePictureWellNeverOpensTheFilePickerButBrowseDoes() {
        App app(load(mediaApp));
        Rig rig(app);
        QVERIFY(app.open());
        QQuickItem *input = find(app.window, "appImageInput-image");
        QVERIFY(input);
        input->setProperty("usePortal", false); // Qt's own dialog, in the window, never the desktop's
        QQuickItem *well = find(app.window, "appInput-image");
        QVERIFY(well);
        tap(app.window, well);
        QTRY_VERIFY(well->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Return);
        QTest::keyClick(app.window, Qt::Key_Space);
        QTest::qWait(100);
        QCOMPARE(input->property("pickerRoute").toString(), QString()); // nothing opened
        tap(app.window, find(app.window, "appImageBrowse-image"));
        QTRY_COMPARE(input->property("pickerRoute").toString(), QStringLiteral("qt"));
        QTest::keyClick(app.window, Qt::Key_Escape);
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

    // The rail: Apps opens the shelf over the page and choosing an app opens it as a new tab; Results opens the library; Graph is
    // Open in Graph. A new tab with nothing in it, made over an app, is "landing" (the shelf shows there, #388).
    void theRailOpensTheShelfAndTheLibraryAndANewTabOverAnAppLandsOnTheShelf() {
        App app(load(appWorkflow));
        Rig rig(app);
        app.workflows.setTemplateRoots({{QStringLiteral(FUGUE_SOURCE_DIR "/tests/apps/templates"), false}});
        QVERIFY(app.open());
        QTRY_VERIFY(find(app.window, "appRail"));
        QCOMPARE(app.workflows.count(), 1);

        tap(app.window, find(app.window, "appRailResults"));
        QTRY_VERIFY(app.window->findChild<QObject *>("appLibrary")->property("visible").toBool());
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!app.window->findChild<QObject *>("appLibrary")->property("visible").toBool());

        tap(app.window, find(app.window, "appRailApps"));
        QTRY_VERIFY(app.window->findChild<QObject *>("appsDialog") && app.window->findChild<QObject *>("appsDialog")->property("visible").toBool());
        QTRY_VERIFY(!rig.shelf.apps().isEmpty());
        QVERIFY(rig.shelf.choose("test-app"));
        QTRY_COMPARE(app.workflows.count(), 2); // a new tab, the first one kept
        QTRY_COMPARE(find(app.window, "appTitle")->property("title").toString(), QStringLiteral("Block title"));

        app.workflows.newWorkflow();
        QCOMPARE(app.workflows.count(), 3);
        QVERIFY(app.workflows.currentIsPristine()); // the shelf's "landing", though other tabs are open
        qDeleteAll(app.engine.rootObjects());
    }

    // The inputs panel (owner, 2026-11-25): its border resizes it and the width is kept; the hero's hide button slides it shut to a
    // strip whose button brings it back, and Window > App inputs does the same; a drag far past its least width hides it too.
    void theInputsPanelResizesHidesAndComesBack() {
        App app(load(appWorkflow));
        Rig rig(app);
        QVERIFY(app.open());
        QQuickItem *panel = nullptr;
        QTRY_VERIFY((panel = find(app.window, "appPanel")) != nullptr);
        QQuickItem *handle = find(app.window, "appPanelHandle");
        QVERIFY(handle);
        const qreal before = panel->width();

        handle->forceActiveFocus();
        QTest::keyClick(app.window, Qt::Key_Left, Qt::ShiftModifier); // the border moves toward the stage: the panel grows
        QTRY_VERIFY(panel->width() > before);
        QVERIFY(app.sidePanels.appWidth() > 0);
        QTest::keyClick(app.window, Qt::Key_Return); // the automatic width again
        QTRY_COMPARE(panel->width(), before);
        QCOMPARE(app.sidePanels.appWidth(), 0);

        tap(app.window, find(app.window, "appHideInputs"));
        QTRY_VERIFY(find(app.window, "appShowInputs"));
        QVERIFY(!app.chrome.isVisible("app-inputs"));
        QTRY_VERIFY(panel->width() < before / 2);
        QTRY_VERIFY(!find(app.window, "appForm")); // once it has slid shut
        QVERIFY(find(app.window, "appStripRun"));
        QVERIFY(!find(app.window, "appPanelHandle"));

        tap(app.window, find(app.window, "appShowInputs"));
        QTRY_VERIFY(find(app.window, "appForm"));
        QTRY_COMPARE(panel->width(), before);

        QVERIFY(app.commands.trigger("view.toggle-app-inputs"));
        QTRY_VERIFY(find(app.window, "appShowInputs"));
        QVERIFY(app.commands.trigger("view.toggle-app-inputs"));
        QTRY_VERIFY(find(app.window, "appPanelHandle"));

        // A drag of the border to the window's end edge hides the panel.
        handle = find(app.window, "appPanelHandle");
        QTRY_VERIFY(handle->isVisible());
        const QPoint from = handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2)).toPoint();
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        for (int x = from.x(); x < app.window->width() - 2; x += 20)
            QTest::mouseMove(app.window, QPoint(x, from.y()));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, QPoint(app.window->width() - 2, from.y()));
        QTRY_VERIFY(find(app.window, "appShowInputs"));
        qDeleteAll(app.engine.rootObjects());
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
        QCOMPARE(find(app.window, "appTitle")->property("title").toString(), QStringLiteral("Restyle"));
        QVERIFY(!find(app.window, "appCancel"));
        QVERIFY(find(app.window, "appViewSwitch")); // the way to the graph, in the window bar
        QVERIFY(!find(app.window, "sideTabStrip")); // the page's rail is the only rail: no side strip beside it

        // Typing in the form is the value a run binds; Run, the key and the button, reaches the app, not the graph.
        QQuickItem *prompt = find(app.window, "appInput-prompt-edit"); // a prompt is several lines
        QVERIFY(prompt);
        QCOMPARE(prompt->property("text").toString(), QStringLiteral("old words"));
        prompt->setProperty("text", "a red fox");
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
        QTRY_VERIFY(find(app.window, "appResultViewer")); // the picture in the stage
        QTRY_COMPARE(find(app.window, "appTakes")->property("count").toInt(), 1); // and its take in the strip
        QTRY_VERIFY(find(app.window, "appSizeLine")->property("text").toString().startsWith(QStringLiteral("8 × 8 · "))); // fitted (the percentage is the stage's size)
        // Compare with B placed inside A (the block's `compare`): B's picture sits at its rect of A's frame, and without one fills it.
        {
            QQuickItem *viewer = find(app.window, "appResultViewer");
            QVERIFY(viewer);
            viewer->setProperty("mode", "slider");
            QQuickItem *layerB = find(app.window, "layerB");
            QVERIFY(layerB && !layerB->childItems().isEmpty());
            QQuickItem *picture = layerB->childItems().first();
            QTRY_COMPARE(picture->width(), 8 * viewer->property("imageScale").toReal()); // unset: A's whole frame
            viewer->setProperty("placeB", QRectF(0, 2, 8, 4));
            const qreal scale = viewer->property("imageScale").toReal();
            QTRY_COMPARE(picture->height(), 4 * scale);
            QCOMPARE(picture->width(), 8 * scale);
            QCOMPARE(picture->y(), viewer->property("imageY").toReal() + 2 * scale);
            QCOMPARE(picture->x(), viewer->property("imageX").toReal());
            // 100% is of the larger picture: B placed at 8 x 4 over a 16 x 8 file is denser than A (2 file pixels per A pixel).
            viewer->setProperty("sizeB", QSizeF(16, 8));
            QCOMPARE(viewer->property("bLarger").toBool(), true);
            QMetaObject::invokeMethod(viewer, "actualSize");
            QCOMPARE(viewer->property("actualB").toBool(), true);
            QCOMPARE(viewer->property("imageScale").toReal(), 2.0);
            viewer->setProperty("sizeB", QSizeF(4, 2)); // sparser than A: A's pixels are the 100%
            QMetaObject::invokeMethod(viewer, "fit");
            QMetaObject::invokeMethod(viewer, "actualSize");
            QCOMPARE(viewer->property("actualB").toBool(), false);
            QCOMPARE(viewer->property("imageScale").toReal(), 1.0);
            QMetaObject::invokeMethod(viewer, "fit");
            viewer->setProperty("sizeB", QSizeF(0, 0));
            viewer->setProperty("placeB", QRectF());
            viewer->setProperty("mode", "single");
        }
        if (const QByteArray dir = qgetenv("FUGUE_SHOT_DIR"); !dir.isEmpty()) { // FUGUE_SHOT_DIR=<dir>: a picture of the view
            QTest::qWait(300);
            test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/app-view.png");
        }
        tap(app.window, find(app.window, "appRun"));
        QCOMPARE(started.size(), 2);
        emit service.event("r2", {{"event", "run.done"}, {"status", "cancelled"}});

        // The rail's Graph item is view.toggle-app: the graph (and the side strip with it), and the switch back.
        tap(app.window, find(app.window, "appOpenInGraph"));
        QTRY_VERIFY(find(app.window, "canvas"));
        QTRY_VERIFY(find(app.window, "sideTabStrip"));
        tap(app.window, find(app.window, "appViewSwitchApp"));
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(!find(app.window, "sideTabStrip"));
        // Open in Graph: this tab shows the graph; the window bar's App | Graph switch (also view.toggle-app) goes back and forth.
        tap(app.window, find(app.window, "appOpenInGraph"));
        QTRY_VERIFY(find(app.window, "canvas"));
        QTRY_VERIFY(!find(app.window, "appPage"));
        QVERIFY(find(app.window, "appViewSwitch"));
        QVERIFY(!find(app.window, "appShowApp")); // the strip under the canvas is gone
        app.commands.run("run.start"); // the graph's own run now
        QCOMPARE(started.size(), 2);
        tap(app.window, find(app.window, "appViewSwitchApp"));
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(!find(app.window, "canvas"));
        tap(app.window, find(app.window, "appViewSwitchGraph"));
        QTRY_VERIFY(find(app.window, "canvas"));
        QVERIFY(app.commands.run("view.toggle-app"));
        QTRY_VERIFY(find(app.window, "appPage"));

        // A workflow with no block is the window it was: no page; the switch stays (Graph chosen, App opens the builder).
        app.workflows.newWorkflow();
        QTRY_VERIFY(find(app.window, "canvas"));
        QTRY_VERIFY(!find(app.window, "appPage"));
        QVERIFY(find(app.window, "appViewSwitch"));
        QVERIFY(app.commands.isEnabled("view.toggle-app")); // its App segment is Make app
        qDeleteAll(app.engine.rootObjects());
    }

    void theCopyButtonOfAFailedRunPutsTheMessageAndItsDetailsOnTheClipboard() {
        App app(load(appWorkflow));
        Rig rig(app);
        QVERIFY(app.open());
        QVERIFY(app.commands.run("run.start"));
        const QJsonObject first{{"code", "model_missing"}, {"message", "No model can run this."}, {"hint", "Download one."}};
        const QJsonObject second{{"code", "oom"}, {"message", "Not enough GPU memory (request abc123)."}};
        emit rig.service.event("r1", {{"event", "run.done"}, {"status", "failed"}, {"errors", QJsonArray{first, second}}});
        QTRY_VERIFY(find(app.window, "appErrorCopy"));
        QGuiApplication::clipboard()->clear();
        app.window->grabWindow(); // polish: the click lands on a laid-out window
        tap(app.window, find(app.window, "appErrorCopy"));
        QTRY_COMPARE(QGuiApplication::clipboard()->text(),
                     QStringLiteral("No model can run this. Download one.\nNot enough GPU memory (request abc123)."));
        QTRY_VERIFY(find(app.window, "appErrorCopy")->property("copied").toBool());
        QGuiApplication::clipboard()->clear();
    }

    // Ctrl+Enter is the window's Run, also with the cursor in a prompt or a one-line field (#385 left this unverified), and
    // Tab leaves a prompt instead of typing a tab.
    void runIsReachableFromAFocusedTextFieldAndTabLeavesAPrompt() {
        App app(load(appWorkflow));
        RunSetup setup;
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

        QQuickItem *prompt = find(app.window, "appInput-prompt-edit");
        QQuickItem *title = find(app.window, "appInput-title");
        QVERIFY(prompt && title);
        prompt->forceActiveFocus();
        QTRY_VERIFY(prompt->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(started.size(), 1);
        emit service.event("r1", {{"event", "run.done"}, {"status", "ok"}});
        QTRY_VERIFY(!find(app.window, "appCancel"));
        QCOMPARE(prompt->property("text").toString(), QStringLiteral("old words")); // no newline typed

        QTest::keyClick(app.window, Qt::Key_Tab);
        QTRY_VERIFY(!prompt->hasActiveFocus());
        QCOMPARE(prompt->property("text").toString(), QStringLiteral("old words")); // no tab typed
        title->forceActiveFocus();
        QTRY_VERIFY(title->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(started.size(), 2);
        qDeleteAll(app.engine.rootObjects());
    }

    // Takes of an earlier session (#389) and Live (#471) in the real window: the strip has the runs of History for this file, their
    // pictures come by replay when the strip draws them, and Live is a switch Space flips. FUGUE_SHOT_DIR=<dir> also takes a picture.
    void earlierTakesComeBackAndLiveIsASwitch() {
        App app(load(lookApp), {}, Look{qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("tokyo-night")), 12});
        Rig rig(app);
        QVERIFY(app.open());
        QTemporaryDir appFolder, pictures;
        QDir(appFolder.path()).mkpath("inputs");
        QVERIFY(QImage(QStringLiteral(FUGUE_SOURCE_DIR "/templates/flux2-klein-9b-da-restore/inputs/reading-nook.jpg")).save(appFolder.filePath("inputs/sample.png")));
        QFile workflow(appFolder.filePath("workflow.fugue"));
        QVERIFY(workflow.open(QIODevice::WriteOnly));
        workflow.write(lookApp);
        workflow.close();
        const QString file = appFolder.filePath("workflow.fugue");
        // Three runs of an earlier session, as the run service leaves them: the bound copy with the form, the result, the end.
        const QStringList prompts{"an old family portrait", "a wedding in 1950", "two children at the sea"};
        for (int i = 0; i < prompts.size(); ++i) {
            QImage image(1392, 752, QImage::Format_RGB32);
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    image.setPixelColor(x, y, QColor::fromHsv((60 + 110 * i + x * 90 / image.width() + y * 40 / image.height()) % 360, 120, 90 + 120 * y / image.height()));
            const QString picture = pictures.filePath(QString("take%1.png").arg(i));
            QVERIFY(image.save(picture));
            const Result<RunFolder> folder = RunFolder::create(app.dir.path() + "/runs", QString("20261125-00000%1-000-aaa%1").arg(i + 1));
            QVERIFY(folder.ok());
            QJsonObject bound = QJsonDocument::fromJson(lookApp).object();
            bound.insert(app::takeKey, app::takeRecord(file, {{"prompt", prompts.at(i)}, {"steps", 2 + i % 2}, {"seed", 1000 + 7 * i}},
                                                       {{"seed", "fixed"}}));
            QVERIFY(folder->writeWorkflow(bound).isEmpty());
            QVERIFY(folder->queued(file));
            folder->appendEvent({{"event", "node.done"}, {"node", "save"},
                                 {"shown", QJsonObject{{"image", QJsonObject{{"type", "image"}, {"path", picture}}}}}});
            folder->finish(RunStatus::Ok, {}, QDateTime::currentDateTimeUtc());
        }
        QVERIFY(app.workflows.openPath(file));
        QTRY_VERIFY(find(app.window, "appPage"));
        QTRY_COMPARE(rig.view.takes().size(), 3);
        QTRY_VERIFY([&rig] {
            const QVariantList takes = rig.view.takes();
            return std::all_of(takes.cbegin(), takes.cend(), [](const QVariant &take) { return !take.toMap().value("url").toString().isEmpty(); });
        }());
        QVERIFY(rig.view.shownInputs().contains("two children at the sea")); // the newest
        // Run again: one chooser (Off, On change, Repeat), arrows move it; off at first.
        QCOMPARE(rig.view.liveMode(), QStringLiteral("off"));
        QQuickItem *live = find(app.window, "appLiveMode");
        QVERIFY(live);
        live->forceActiveFocus();
        QTRY_VERIFY(live->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Right);
        QTRY_COMPARE(rig.view.liveMode(), QStringLiteral("live"));
        QTest::keyClick(app.window, Qt::Key_Left);
        QTRY_COMPARE(rig.view.liveMode(), QStringLiteral("off"));
        QTest::keyClick(app.window, Qt::Key_Right);
        QTRY_COMPARE(rig.view.liveMode(), QStringLiteral("live"));
        QCOMPARE(rig.started.size(), 0); // Live alone starts nothing until an input changes (Repeat would start at once)
        // R on a take runs it again with its inputs.
        QQuickItem *strip = find(app.window, "appTakes");
        QVERIFY(strip);
        strip->forceActiveFocus();
        QTRY_VERIFY(strip->hasActiveFocus());
        strip->setProperty("currentIndex", 2);
        QTest::keyClick(app.window, Qt::Key_R);
        QTRY_COMPARE(rig.started.size(), 1);
        QCOMPARE(rig.started.last().document.graph.nodes.value("encode").inputs.value("prompt").toJson(), QJsonValue("an old family portrait"));
        QCOMPARE(rig.started.last().document.graph.nodes.value("sample").inputs.value("seed").toJson(), QJsonValue(1000));
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (!dir.isEmpty()) {
            app.window->resize(1888, 971);
            QTest::qWait(500);
            QDir().mkpath(QString::fromLocal8Bit(dir));
            test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/app-takes-live.png");
        }
        qDeleteAll(app.engine.rootObjects());
    }

    // FUGUE_SHOT_DIR=<dir>: the page as the owner sees it at two window sizes, with a result, takes, a running run and the Apps
    // dialog. Nothing is checked beyond the pieces being there; it is for looking at the pictures.
    void theLookInTwoSizes() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to take the pictures");
        // FUGUE_SHOT_THEME=<omarchy theme> (default tokyo-night): one theme per process, a second window in one process is not safe.
        for (const QString &themeName : {qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("tokyo-night"))}) {
        // FUGUE_SHOT_LOOK="preset=flat;look_corners=1;frost=0": look settings applied before the pictures; FUGUE_SHOT_TAG names the folder.
        const QString tag = qEnvironmentVariable("FUGUE_SHOT_TAG");
        const QString out = QString::fromLocal8Bit(dir) + "/" + themeName + (tag.isEmpty() ? QString() : "-" + tag);
        QDir().mkpath(out);
        App app(load(lookApp), {}, Look{themeName, 12});
        Rig rig(app);
        app.workflows.setTemplateRoots({{QStringLiteral(FUGUE_SOURCE_DIR "/tests/apps/templates"), false}});
        QVERIFY(app.open());
        for (const QString &part : qEnvironmentVariable("FUGUE_SHOT_LOOK").split(';', Qt::SkipEmptyParts)) {
            const QStringList kv = part.split('=');
            QVERIFY(kv.size() == 2);
            QCOMPARE(kv[0] == "preset" ? app.settings.applyLookPreset(kv[1]) : app.settings.set(kv[0], kv[1].toDouble()), QString());
        }
        // The app as a file with a real template's preview and a sample picture beside it, as an app of the catalog is.
        QTemporaryDir appFolder;
        QDir(appFolder.path()).mkpath("inputs");
        if (qEnvironmentVariableIsSet("FUGUE_SHOT_PALE")) { // FUGUE_SHOT_PALE=1: a pale preview, for the frosted buttons' dark ink
            QImage pale(640, 400, QImage::Format_RGB32);
            for (int y = 0; y < pale.height(); ++y)
                for (int x = 0; x < pale.width(); ++x)
                    pale.setPixelColor(x, y, QColor::fromHsv(40 + x * 40 / pale.width(), 25, 235 + 20 * y / pale.height()));
            QVERIFY(pale.save(appFolder.filePath("preview.png")));
        } else {
            QVERIFY(QFile::copy(QStringLiteral(FUGUE_SOURCE_DIR "/templates/flux2-klein-9b-da-restore/preview.webp"), appFolder.filePath("preview.webp")));
        }
        QVERIFY(QImage(QStringLiteral(FUGUE_SOURCE_DIR "/templates/flux2-klein-9b-da-restore/inputs/reading-nook.jpg")).save(appFolder.filePath("inputs/sample.png")));
        QFile workflow(appFolder.filePath("workflow.fugue"));
        QVERIFY(workflow.open(QIODevice::WriteOnly));
        workflow.write(lookApp);
        workflow.close();
        QVERIFY(app.workflows.openPath(appFolder.filePath("workflow.fugue")));
        QTRY_VERIFY(find(app.window, "appPage"));
        // The Advanced inputs open, so a switch shows (FUGUE_SHOT_TOGGLE=0 for it off).
        rig.view.setValue("tapered", qEnvironmentVariable("FUGUE_SHOT_TOGGLE", "1") != "0");
        find(app.window, "appPage")->setProperty("advancedOpen", true);
        QTemporaryDir files;
        const auto made = [&files](const QString &name, int w, int h, int hue) {
            QImage image(w, h, QImage::Format_RGB32);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    image.setPixelColor(x, y, QColor::fromHsv((hue + x * 120 / w + y * 60 / h) % 360, 120, 90 + 120 * y / h));
            image.save(files.filePath(name));
            return files.filePath(name);
        };
        const QString result = made("restored.png", 1392, 752, 20);
        const QString first = made("first.png", 1392, 752, 200);
        const auto finish = [&](const QString &run, const QString &file) {
            emit rig.service.event(run, {{"event", "run.started"}});
            emit rig.service.event(run, {{"event", "node.done"}, {"node", "save"},
                                         {"thumbnails", QJsonObject{{"image", QJsonObject{{"url", QUrl::fromLocalFile(file).toString()}, {"source", file},
                                                                                          {"width", 1392}, {"height", 752}}}}}});
            emit rig.service.event(run, {{"event", "run.done"}, {"status", "ok"}});
        };
        rig.view.run();
        finish("r1", first);
        rig.view.setValue("steps", 2);
        rig.view.run();
        finish("r2", result);
        rig.view.setValue("steps", 4);
        rig.view.run(); // running: r3
        emit rig.service.event("r3", {{"event", "run.started"}});
        emit rig.service.event("r3", {{"event", "node.progress"}, {"value", 0.4}});
        for (const QSize size : {QSize(1888, 971), QSize(900, 700)}) {
            app.window->resize(size);
            QTest::qWait(400);
            test_support::grab(app.window).save(QString("%1/app-running-%2x%3.png").arg(out).arg(size.width()).arg(size.height()));
        }
        finish("r3", made("third.png", 1392, 752, 120));
        QTRY_COMPARE(find(app.window, "appTakes")->property("count").toInt(), 3);
        for (const QSize size : {QSize(1888, 971), QSize(900, 700)}) {
            app.window->resize(size);
            QTest::qWait(400);
            test_support::grab(app.window).save(QString("%1/app-result-%2x%3.png").arg(out).arg(size.width()).arg(size.height()));
        }
        // What the app still needs (#469): the quiet panel above the inputs.
        rig.view.needs()->setPlan(needsPlan());
        for (const QSize size : {QSize(1888, 971), QSize(900, 700)}) {
            app.window->resize(size);
            QTest::qWait(400);
            test_support::grab(app.window).save(QString("%1/app-needs-%2x%3.png").arg(out).arg(size.width()).arg(size.height()));
        }
        rig.view.needs()->setPlan(ReadinessPlan());
        // Zoomed into the picture, the stage's own controls shown.
        QQuickItem *viewer = find(app.window, "appResultViewer");
        QVERIFY(viewer);
        QMetaObject::invokeMethod(viewer, "zoomAt", Q_ARG(QVariant, viewer->width() / 2), Q_ARG(QVariant, viewer->height() / 2), Q_ARG(QVariant, 3.0));
        QTest::qWait(300);
        test_support::grab(app.window).save(QString("%1/app-zoomed.png").arg(out));
        // The Apps dialog from the rail.
        app.window->resize(1888, 971);
        tap(app.window, find(app.window, "appRailApps"));
        QTRY_VERIFY(find(app.window, "appsDialog") || app.window->findChild<QObject *>("appsDialog"));
        QTest::qWait(500);
        test_support::grab(app.window).save(QString("%1/app-apps-dialog.png").arg(out));
        // Right to left: the rows mirror (rail and stage to the end side, the panel to the start); the picture does not.
        QTest::keyClick(app.window, Qt::Key_Escape);
        QGuiApplication::setLayoutDirection(Qt::RightToLeft);
        app.window->resize(1200, 760);
        QTest::qWait(500);
        test_support::grab(app.window).save(QString("%1/app-rtl.png").arg(out));
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
        // The graph, with Fugue's mark at the window bar's start.
        app.window->resize(1888, 971);
        QVERIFY(app.commands.run("view.toggle-app"));
        QTRY_VERIFY(find(app.window, "canvas"));
        QTest::qWait(400);
        test_support::grab(app.window).save(QString("%1/app-graph-corner.png").arg(out));
        qDeleteAll(app.engine.rootObjects());
        }
    }


    // The panel by keys alone (owner rule): focus the first input, Down opens the picture menu and Escape closes it, Tab goes through
    // the panel in reading order to Run, Ctrl+Enter runs from the first input, Enter on the focused Run runs, the takes strip takes
    // the arrows, and the Apps shelf (Ctrl+Shift+A, app.show-all) opens on a card, Enter opens it and Escape closes the dialog.
    void thePanelWorksByKeysAlone() {
        App app(load(lookApp));
        Rig rig(app);
        app.workflows.setTemplateRoots({{QStringLiteral(FUGUE_SOURCE_DIR "/tests/apps/templates"), false}});
        QVERIFY(app.open());
        QQuickItem *first = find(app.window, "appImageChoose-image");
        QVERIFY(first);
        const QString picture = app.dir.path() + "/result.png";
        QVERIFY(QImage(8, 8, QImage::Format_RGB32).save(picture));
        const auto finish = [&](const char *run) {
            emit rig.service.event(run, {{"event", "run.started"}});
            emit rig.service.event(run, {{"event", "node.done"}, {"node", "save"},
                                         {"thumbnails", QJsonObject{{"image", QJsonObject{{"url", QUrl::fromLocalFile(picture).toString()}, {"source", picture}, {"width", 8}, {"height", 8}}}}}});
            emit rig.service.event(run, {{"event", "run.done"}, {"status", "ok"}});
        };
        first->forceActiveFocus();
        QTRY_VERIFY(first->hasActiveFocus());
        QQuickItem *menu = find(app.window, "appImageInput-image");
        QVERIFY(menu);
        QTest::keyClick(app.window, Qt::Key_Down);
        QTRY_VERIFY(menu->property("menuOpen").toBool());
        QTest::keyClick(app.window, Qt::Key_Down); // arrows move in the menu
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("menuOpen").toBool());
        QTRY_VERIFY(first->hasActiveFocus());

        // Ctrl+Enter from the first input runs.
        QTest::keyClick(app.window, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(rig.started.size(), 1);
        finish("r1");
        QTRY_VERIFY(rig.view.canRun());

        // Tab: reading order, the focus ring on a control of the panel each time, ending on Run.
        const auto named = [&](QQuickItem *item) {
            for (; item; item = item->parentItem())
                if (item->objectName().startsWith("app"))
                    return item->objectName();
            return QString();
        };
        QStringList order;
        for (int i = 0; i < 24 && (order.isEmpty() || !order.last().startsWith("appRun")); ++i) {
            QTest::keyClick(app.window, Qt::Key_Tab);
            QQuickItem *focused = app.window->activeFocusItem();
            order << (named(focused) + "/" + (focused ? focused->metaObject()->className() : "null"));
        }
        QVERIFY2(order.last().startsWith(QStringLiteral("appRun")), qPrintable(order.join(",")));
        const auto at = [&](const char *name) {
            for (int i = 0; i < order.size(); ++i)
                if (order.at(i).startsWith(QString::fromLatin1(name) + "/"))
                    return i;
            return -1;
        };
        QVERIFY2(at("appImageBrowse-image") >= 0 && at("appInput-steps") > at("appImageBrowse-image")
                 && at("appSeedRoll-seed") > at("appInput-steps") && at("appAdvanced") > at("appSeedRoll-seed")
                 && at("appRun") > at("appAdvanced"), qPrintable(order.join(',')));
        QTest::keyClick(app.window, Qt::Key_Return); // Enter on the focused Run
        QCOMPARE(rig.started.size(), 2);
        finish("r2");

        // The takes strip: the arrows move, a take shows.
        QQuickItem *takes = nullptr;
        QTRY_VERIFY((takes = find(app.window, "appTakes")));
        takes->forceActiveFocus();
        QTRY_VERIFY(takes->hasActiveFocus());
        QCOMPARE(takes->property("currentIndex").toInt(), 0);
        QTest::keyClick(app.window, Qt::Key_Right);
        QTRY_COMPARE(takes->property("currentIndex").toInt(), 1);
        QTest::keyClick(app.window, Qt::Key_Left);
        QTRY_COMPARE(takes->property("currentIndex").toInt(), 0);

        // All apps: the key opens the shelf with the focus on the search; Escape closes it; Down goes to the cards and Enter opens
        // the focused card.
        QVERIFY(app.commands.run("app.show-all"));
        QObject *dialog = nullptr;
        QTRY_VERIFY((dialog = app.window->findChild<QObject *>("appsDialog")) && dialog->property("visible").toBool());
        QQuickItem *search = nullptr;
        QTRY_VERIFY((search = find(app.window, "appsSearch")) && search->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(app.commands.run("app.show-all"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QQuickItem *grid = nullptr;
        QTRY_VERIFY((search = find(app.window, "appsSearch")) && search->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Down);
        QTRY_VERIFY((grid = find(app.window, "appShelfGrid")) && grid->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_COMPARE(app.workflows.count(), 2);
        qDeleteAll(app.engine.rootObjects());
    }

    // The Needs panel (#469): while the app lacks something it lists each item with its size and licence above the inputs; by keys alone
    // Tab reaches Download all first, then each row's Download; Enter presses; Show in graph jumps to the node; it clears when ready.
    void theNeedsPanelListsWhatIsMissingAndWorksByKeys() {
        App app(load(mediaWithModel()));
        Rig rig(app);
        NeedsModel *needs = rig.view.needs();
        needs->setPlanSource([] { return ReadinessPlan(); }); // the real source would find the made-up model missing: a fake plan instead
        QObject::disconnect(needs, nullptr, &app.workflows, nullptr); // the window's own answers (the sheet, a download) are not this test's
        QVERIFY(app.open());
        QVERIFY(!find(app.window, "appNeeds")); // ready: no panel
        QSignalSpy sheet(needs, &NeedsModel::getReadyRequested);
        QSignalSpy single(needs, &NeedsModel::downloadRequested);
        needs->setPlan(needsPlan());
        QQuickItem *panel = nullptr;
        QTRY_VERIFY((panel = find(app.window, "appNeeds")));
        QVERIFY(find(app.window, "appNeed-model:x.pth"));
        QVERIFY(find(app.window, "appNeed-model:flux-2-klein-9b-Q4_K_M.gguf"));
        QVERIFY(find(app.window, "appNeedsHeadline")->property("text").toString().contains("2 models"));
        // Tab from the top: the header's Download all comes before every input of the form.
        QStringList order;
        const auto named = [](QQuickItem *item) {
            for (; item; item = item->parentItem())
                if (item->objectName().startsWith("app"))
                    return item->objectName();
            return QString();
        };
        for (int i = 0; i < 12 && !order.contains("appNeedsAll"); ++i) {
            QTest::keyClick(app.window, Qt::Key_Tab);
            order << named(app.window->activeFocusItem());
        }
        QVERIFY2(order.contains("appNeedsAll"), qPrintable(order.join(',')));
        QCOMPARE(app.window->activeFocusItem()->objectName(), QStringLiteral("appNeedsAll"));
        QTest::keyClick(app.window, Qt::Key_Return); // models only: the click is the yes
        QCOMPARE(sheet.size(), 1);
        QCOMPARE(sheet.first().first().toBool(), true);
        // The next stops are the rows' buttons; Enter on a model's Download asks for that one.
        QStringList rows;
        for (int i = 0; i < 6 && single.isEmpty(); ++i) {
            QTest::keyClick(app.window, Qt::Key_Tab);
            const QString at = named(app.window->activeFocusItem());
            rows << at;
            if (at.startsWith("appNeedGet-"))
                QTest::keyClick(app.window, Qt::Key_Return);
        }
        QVERIFY2(single.size() == 1, qPrintable(rows.join(',')));
        // Show in graph: the model's node is selected in the graph, which is shown.
        QQuickItem *show = find(app.window, "appNeedShow-model:x.pth");
        QVERIFY(show);
        QVERIFY(!find(app.window, "appNeedShow-model:flux-2-klein-9b-Q4_K_M.gguf")); // in no node
        tap(app.window, show);
        QTRY_VERIFY(!rig.view.shown());
        QCOMPARE(app.model.selectedIds(), QStringList{"up"});
        // Back in the App view, ready now: the panel is gone.
        rig.view.showApp();
        needs->setPlan(ReadinessPlan());
        QTRY_VERIFY(!find(app.window, "appNeeds"));
        qDeleteAll(app.engine.rootObjects());
    }

    // The per-type controls are in the form and one key flips a switch (the values are test_app_view's).
    void everyKindOfInputHasItsControlAndASwitchTakesTheKeyboard() {
        App app(load(mediaApp));
        RunSetup setup;
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
        for (const char *name : {"appInput-image", "appInput-mask", "appInput-tapered", "appInput-model", "appInput-seed",
                                 "appSeedMode-seed", "appImageBrowse-image"})
            QVERIFY2(find(app.window, name), name);
        QQuickItem *toggle = find(app.window, "appInput-tapered");
        toggle->forceActiveFocus();
        QTRY_VERIFY(toggle->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Space);
        QTRY_COMPARE(view.valueOf("tapered").toBool(), false);
        QCOMPARE(toggle->property("checked").toBool(), false);
        QVERIFY(app.commands.run("run.start"));
        QCOMPARE(started.first().document.graph.nodes.value("grow").inputs.value("tapered_corners").toJson(), QJsonValue(false));
        qDeleteAll(app.engine.rootObjects());
    }

    // Ctrl+V with a picture on the clipboard (the offscreen platform's own) fills the picture input; a focused text field keeps it.
    void ctrlVPastesAPictureIntoTheAppUnlessATextFieldHasTheFocus() {
        App app(load(mediaApp));
        Rig rig(app);
        QVERIFY(app.open());
        QTRY_VERIFY(find(app.window, "appInput-image"));
        QVERIFY(rig.view.valueOf("image").toString().isEmpty());
        QImage picture(6, 4, QImage::Format_RGB32);
        picture.fill(Qt::red);
        auto *data = new QMimeData;
        data->setImageData(picture);
        QGuiApplication::clipboard()->setMimeData(data);
        QQuickItem *seed = find(app.window, "appInput-seed");
        QVERIFY(seed);
        seed->forceActiveFocus(); // a text field: the key is its own
        QTRY_VERIFY(seed->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_V, Qt::ControlModifier);
        QTest::qWait(50);
        QVERIFY(rig.view.valueOf("image").toString().isEmpty());
        find(app.window, "appInput-image")->forceActiveFocus(); // the picture well: the page's paste
        QTest::keyClick(app.window, Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(!rig.view.valueOf("image").toString().isEmpty());
        QVERIFY(QFileInfo(rig.view.valueOf("image").toString()).isFile());
        qDeleteAll(app.engine.rootObjects());
    }

    // The performance budget (docs/canvas-look.md 20, Performance): an App view that has settled draws nothing: no animation, timer or
    // effect keeps the scene graph busy, so no frame is swapped for a second.
    // One function per look so the two land in different shards (each waits for the page to settle).
    void anIdleAppViewDoesNotRedrawInCalm() { idleAppViewDoesNotRedraw(QStringLiteral("calm")); }
    void anIdleAppViewDoesNotRedrawInFlat() { idleAppViewDoesNotRedraw(QStringLiteral("flat")); }

private:
    void idleAppViewDoesNotRedraw(const QString &preset) {
        App app(load(mediaApp));
        Rig rig(app);
        QElapsedTimer opened;
        opened.start();
        QVERIFY(app.open());
        QCOMPARE(app.settings.applyLookPreset(preset), QString());
        QTRY_VERIFY(find(app.window, "appInput-seed"));
        qInfo() << "the App view's first frame after open," << preset << ":" << opened.elapsed() << "ms";
        // Wait on the condition, not a fixed time: the page settles when a half second passes with no frame (the preset's colour
        // changes and the frosted controls' measure, 150 ms later, animate once); from then on it must stay so for half a second.
        int frames = 0;
        const auto connection = connect(app.window, &QQuickWindow::frameSwapped, this, [&frames] { ++frames; });
        bool quiet = false;
        for (int i = 0; i < 20 && !quiet; ++i) {
            frames = 0;
            QTest::qWait(500);
            quiet = frames == 0;
        }
        QVERIFY2(quiet, "the App view never stopped drawing");
        QTest::qWait(500);
        disconnect(connection);
        QCOMPARE(frames, 0);
        qDeleteAll(app.engine.rootObjects());
    }

private slots:

    // The seed row: Random and back to Fixed through the control keeps a number in the field; Clear empties a result.
    void theSeedModeControlReachesTheStoreAndClearEmptiesTheResult() {
        App app(load(mediaApp));
        Rig rig(app);
        QVERIFY(app.open());
        QQuickItem *mode = nullptr;
        QTRY_VERIFY((mode = find(app.window, "appSeedMode-seed")));
        QQuickItem *field = find(app.window, "appInput-seed");
        QCOMPARE(field->property("text").toString(), QStringLiteral("3"));
        QCOMPARE(mode->property("currentIndex").toInt(), 0); // Fixed
        // By key: the chooser is one tab stop, Right chooses Random, Left Fixed again; the number field then shows and hides.
        mode->forceActiveFocus();
        QTRY_VERIFY(mode->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Right);
        QTRY_COMPARE(rig.view.seedMode("seed"), QStringLiteral("random"));
        QTRY_COMPARE(field->property("text").toString(), QString());
        QTRY_COMPARE(mode->property("currentIndex").toInt(), 1);
        QTest::keyClick(app.window, Qt::Key_Left);
        QTRY_COMPARE(rig.view.seedMode("seed"), QStringLiteral("fixed"));
        QTRY_COMPARE(field->property("text").toString(), QStringLiteral("3"));
        // By the signal a press makes: Increment.
        QMetaObject::invokeMethod(mode, "moved", Q_ARG(int, 2));
        QTRY_COMPARE(rig.view.seedMode("seed"), QStringLiteral("increment"));
        QMetaObject::invokeMethod(mode, "moved", Q_ARG(int, 0));
        QTRY_COMPARE(rig.view.seedMode("seed"), QStringLiteral("fixed"));
        QCOMPARE(mode->property("currentIndex").toInt(), 0);

        QVERIFY(!find(app.window, "appClear"));
        const QString picture = app.dir.path() + "/a.png"; // a real file: the stage and the strip load it
        QVERIFY(QImage(4, 4, QImage::Format_RGB32).save(picture));
        rig.view.run();
        rig.view.onEvent("r1", QJsonObject{{"event", "run.started"}});
        rig.view.onEvent("r1", QJsonObject{{"event", "node.done"}, {"node", "save"},
                                           {"thumbnails", QJsonObject{{"image", QJsonObject{{"url", QUrl::fromLocalFile(picture).toString()}, {"source", picture},
                                                                                            {"width", 4}, {"height", 4}}}}}});
        QVERIFY(!find(app.window, "appClear")); // the run still goes: Cancel is in the toolbar's place
        QVERIFY(find(app.window, "appCancel"));
        rig.view.onEvent("r1", QJsonObject{{"event", "run.done"}, {"status", "ok"}});
        QQuickItem *clear = nullptr;
        QTRY_VERIFY((clear = find(app.window, "appClear")));
        QVERIFY(clear->isEnabled());
        QTRY_VERIFY(find(app.window, "appReuseInputs")); // the toolbar has all its buttons: the layout is settled before the press
        QPointF where;
        QTRY_VERIFY((where == clear->mapToScene(QPointF())) || ((where = clear->mapToScene(QPointF())) != QPointF() && false)); // stopped moving
        QCOMPARE(rig.view.results().size(), 1);
        // The toolbar can still be settling on a cold start (the first run after a build compiles the QML): press again until it takes.
        for (int attempt = 0; attempt < 5 && !rig.view.results().isEmpty(); ++attempt) {
            if (QQuickItem *button = find(app.window, "appClear"))
                tap(app.window, button);
            QTest::qWait(300);
        }
        QTRY_VERIFY(rig.view.results().isEmpty());
        QTRY_VERIFY(!find(app.window, "appClear"));
        QVERIFY(rig.view.takes().isEmpty()); // Clear empties the stage and the strip (owner, 2026-11-25)
        QCOMPARE(rig.view.takeIndex(), -1);
        QVERIFY(!find(app.window, "appTakes"));
        qDeleteAll(app.engine.rootObjects());
    }

    // The mask input of the form: Paint mask opens the editor on the form's picture, a stroke shows in the editor's layer, and OK
    // sets the input to the stored file, which the run then binds (owner, 2026-11-25: "mask painting does not create or show the mask").
    void paintingAMaskInTheFormShowsTheStrokeAndSetsTheInput() {
        App app(load(mediaApp));
        Rig rig(app);
        QVERIFY(app.open());
        QTRY_VERIFY(find(app.window, "appInput-mask"));
        QTemporaryDir files;
        QImage picture(200, 150, QImage::Format_RGB32);
        picture.fill(Qt::gray);
        QVERIFY(picture.save(files.filePath("photo.png")));
        rig.view.setValue("image", files.filePath("photo.png"));
        QCOMPARE(rig.view.maskPicture().value("width").toInt(), 200);
        paintAMask(app, rig);
        qDeleteAll(app.engine.rootObjects());
    }

private:
    // Paint mask is pressed, a stroke is dragged over the picture and OK is taken: the stroke is in the editor's layer and the input
    // holds the stored mask, which the form says is Painted.
    void paintAMask(App &app, Rig &rig) {
        QPointF where; // the form settles as the picture loads: press where the button stopped
        QTRY_VERIFY((where == find(app.window, "appInput-mask")->mapToScene(QPointF())) || ((where = find(app.window, "appInput-mask")->mapToScene(QPointF())) != QPointF() && false));
        tap(app.window, find(app.window, "appInput-mask"));
        QObject *editor = nullptr; // a Popup, not an item: the form's Loader holds it
        const auto openEditor = [&] {
            for (QQuickItem *child : find(app.window, "appInput-mask")->parentItem()->childItems())
                if (QObject *made = child->property("item").value<QObject *>())
                    editor = made;
            return editor != nullptr;
        };
        QTRY_VERIFY(openEditor() && editor->property("opened").toBool());
        const QSizeF size = editor->property("imageSize").toSizeF();
        QCOMPARE(size, QSizeF(rig.view.maskPicture().value("width").toInt(), rig.view.maskPicture().value("height").toInt()));
        QVERIFY(size.width() > 0);
        auto *layer = editor->property("layer").value<fugue::MaskLayer *>();
        QVERIFY(layer);
        QTRY_VERIFY(layer->zoom() > 0);
        editor->setProperty("brushSize", qMax(4.0, size.height() / 8));
        QTest::qWait(60);
        const auto at = [&](double x, double y) {
            return layer->mapToScene(QPointF(layer->originX() + x * layer->zoom(), layer->originY() + y * layer->zoom())).toPoint();
        };
        const QPoint middle = at(size.width() / 2, size.height() / 2);
        const QImage before = test_support::grab(app.window);
        QTest::mousePress(app.window, Qt::LeftButton, {}, at(size.width() / 4, size.height() / 2));
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(app.window, at(size.width() * (0.25 + 0.05 * i), size.height() / 2));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, at(size.width() * 0.65, size.height() / 2));
        QVERIFY(!layer->isEmpty());
        QTest::qWait(60);
        QVERIFY2(test_support::grab(app.window).pixel(middle) != before.pixel(middle), "the stroke is not drawn over the picture");
        if (const QByteArray dir = qgetenv("FUGUE_SHOT_DIR"); !dir.isEmpty()) { // the editor as the form opens it, to look at
            QTest::qWait(100);
            test_support::grab(app.window).save(QString("%1/app-mask-editor.png").arg(QString::fromLocal8Bit(dir)));
        }
        QCOMPARE(layer->valueAt(int(size.width() / 2), int(size.height() / 2)), 255);
        QMetaObject::invokeMethod(editor, "accept");
        const QString stored = rig.view.valueOf("mask").toString();
        QVERIFY2(!stored.isEmpty(), "OK left the mask input empty");
        QVERIFY(QFileInfo(stored).isFile());
        QTRY_COMPARE(find(app.window, "appMaskState-mask")->property("text").toString(), QStringLiteral("Painted"));
    }
};

FUGUE_TEST_MAIN(TestAppWindow, QGuiApplication)
#include "test_app_window.moc"
