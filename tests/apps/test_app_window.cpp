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
#include "apps/app_window.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

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

        // A workflow with no block is the window it was: no page, no switch.
        app.workflows.newWorkflow();
        QTRY_VERIFY(find(app.window, "canvas"));
        QTRY_VERIFY(!find(app.window, "appPage"));
        QTRY_VERIFY(!find(app.window, "appViewSwitch"));
        QVERIFY(!app.commands.isEnabled("view.toggle-app")); // nothing to switch
        qDeleteAll(app.engine.rootObjects());
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

        // All apps: the key opens the shelf with the focus on the cards; Escape closes it; Enter opens the focused card.
        QVERIFY(app.commands.run("app.show-all"));
        QObject *dialog = nullptr;
        QTRY_VERIFY((dialog = app.window->findChild<QObject *>("appsDialog")) && dialog->property("visible").toBool());
        QQuickItem *grid = nullptr;
        QTRY_VERIFY((grid = find(app.window, "appShelfGrid")) && grid->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(app.commands.run("app.show-all"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_VERIFY((grid = find(app.window, "appShelfGrid")) && grid->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_COMPARE(app.workflows.count(), 2);
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
    void anIdleAppViewDoesNotRedraw() {
        App app(load(mediaApp));
        Rig rig(app);
        QElapsedTimer opened;
        opened.start();
        QVERIFY(app.open());
        QTRY_VERIFY(find(app.window, "appInput-seed"));
        qInfo() << "the App view's first frame after open:" << opened.elapsed() << "ms";
        // Wait on the condition, not a fixed time: the page settles when a half second passes with no frame; from then on it must stay so.
        int frames = 0;
        const auto connection = connect(app.window, &QQuickWindow::frameSwapped, this, [&frames] { ++frames; });
        bool quiet = false;
        for (int i = 0; i < 20 && !quiet; ++i) {
            frames = 0;
            QTest::qWait(500);
            quiet = frames == 0;
        }
        QVERIFY2(quiet, "the App view never stopped drawing");
        QTest::qWait(1000);
        disconnect(connection);
        QCOMPARE(frames, 0);
        qDeleteAll(app.engine.rootObjects());
    }

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
        rig.view.run();
        rig.view.onEvent("r1", QJsonObject{{"event", "run.started"}});
        rig.view.onEvent("r1", QJsonObject{{"event", "node.done"}, {"node", "save"},
                                           {"thumbnails", QJsonObject{{"image", QJsonObject{{"url", "file:///t/a.png"}, {"source", "/t/a.png"},
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
        QCOMPARE(rig.view.takes().size(), 1); // Clear empties the stage; the take stays in the strip
        QCOMPARE(rig.view.takeIndex(), -1);
        rig.view.selectTake(0);
        QCOMPARE(rig.view.shownResults().size(), 1);
        qDeleteAll(app.engine.rootObjects());
    }
};

QTEST_MAIN(TestAppWindow)
#include "test_app_window.moc"
