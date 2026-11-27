// An app as its own application (#398, apps/AppWindow.qml, offscreen, QTest events only): the window is the App view and nothing
// else, its title is the app's, Add to launcher and Remove from launcher write and take away an entry in a scratch data folder,
// Open in Fugue asks for the full window, and the Get ready sheet of #379 opens inside it. The entry's file is
// test_launcher_entries; the store's logic is test_app_view.

#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickStyle>
#include <QtTest>

#include "../test_paths.h"
#include "../canvas/canvas_app.h"
#include "../window_grab.h"
#include "apps/app_launcher.h"
#include "apps/app_shelf.h"
#include "apps/app_window.h"
#include "engine/share_package.h"
#include "engine/templates.h"
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

void tap(QQuickWindow *window, QQuickItem *item) {
    QTest::mouseClick(window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
}

const QString templates = (fugueSourceDir() + QStringLiteral("/tests/apps/templates"));

// A template folder that is an app: a prompt in, a picture out.
void makeApp(const QString &root, const QString &id, const QString &title) {
    QDir().mkpath(root + "/" + id);
    QJsonObject nodes{{"encode", QJsonObject{{"type", "fugue/text-encode"}, {"version", "1.0.0"}, {"inputs", QJsonObject{{"prompt", "a lake"}}}}},
                      {"save", QJsonObject{{"type", "fugue/save-image"}, {"version", "1.1.0"}, {"inputs", QJsonObject{{"name", "out"}}}}}};
    const QJsonObject flow{{"fugue", 1},
                           {"app", QJsonObject{{"format", 1}, {"title", title}, {"defaultView", "app"},
                                               {"inputs", QJsonArray{QJsonObject{{"role", "prompt"}, {"targets", QJsonArray{"encode.prompt"}}, {"label", "Prompt"}}}},
                                               {"outputs", QJsonArray{QJsonObject{{"node", "save"}}}}}},
                           {"graph", QJsonObject{{"nodes", nodes}}}};
    const auto write = [&root, &id](const char *name, const QJsonObject &json) {
        QFile file(root + "/" + id + "/" + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(json).toJson());
    };
    write("workflow.fugue", flow);
    write("template.json", QJsonObject{{"title", title}, {"description", "Makes " + title}, {"app", QJsonObject{{"title", title}, {"summary", "Does " + title}}}});
}

// What main() ties for an app's window: the runs (never started), the view and the launcher over a scratch data folder.
struct Rig {
    RunSetup setup;
    RunService service;
    CanvasRuns runs;
    AppView view;
    QTemporaryDir data;
    QList<QStringList> spawned;
    AppLauncher launcher;
    AppShelf shelf;
    QStringList icons; // the previews the window was given

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
    explicit Rig(App &app, const QString &root = templates)
        : setup(setupIn(app.dir.path())), service(setup), runs(&app.model, &app.workflows, &service, &app.registry),
          view([](const RunRequest &) { return QStringLiteral("r1"); }, [](const QString &) {}, &app.registry),
          launcher(&view, &app.workflows, {{root, false}}, &app.registry, LauncherEntries(data.path(), "fugue"),
                   [this](const QStringList &arguments) { spawned << arguments; return true; }),
          shelf([&app] { return app.workflows.templates(); }, [&app](const QString &id) { return app.workflows.openTemplateById(id); }) {
        app.workflows.setTemplateRoots({{root, false}});
        app.appShelf = &shelf;
        bindOwnAppWindow(shelf, view, launcher,
                         [root, &app](const QString &id) { return resolveApp(id, {{root, false}}, &app.registry); },
                         [this](const AppRef &chosen) { icons << chosen.ref; });
        runs.registerCommands(app.commands);
        app.runs = &runs;
        bindAppView(view, app.workflows, app.model, service, app.commands, runs);
        app.appView = &view;
        app.appLauncher = &launcher;
        QObject::connect(&launcher, &AppLauncher::notice, &app.workflows, &Workflows::notify);
        app.windowUrl = QUrl("qrc:/apps/AppWindow.qml");
    }
};

} // namespace

class TestAppOwnWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theWindowIsTheAppPageAloneAndTheLauncherEntryComesAndGoes() {
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app);
        QVERIFY(app.workflows.openTemplateById("test-app"));
        rig.view.showApp();
        QVERIFY(app.open());

        // The page, and none of the editing chrome.
        QTRY_VERIFY(find(app.window, "appPage"));
        for (const char *chrome : {"menuBar", "canvas", "areas", "areaHost", "appViewSwitch"})
            QVERIFY2(!item(app.window->contentItem(), chrome), chrome);
        QVERIFY(!find(app.window, "appOpenInGraph")); // no graph to open
        QCOMPARE(app.window->title(), QStringLiteral("Block title")); // the app's own
        QVERIFY(find(app.window, "appInput-prompt"));
        QVERIFY(find(app.window, "appRun"));

        // Add to launcher: an entry that starts this app, in the scratch folder only.
        QTRY_VERIFY(find(app.window, "appLauncherToggle"));
        QQuickItem *toggle = find(app.window, "appLauncherToggle");
        QCOMPARE(toggle->property("glyph").toString(), QStringLiteral("app-window"));
        QVERIFY(!toggle->property("current").toBool());
        QVERIFY(toggle->property("tip").toString().startsWith("Add to the app launcher"));
        tap(app.window, toggle);
        const QString entry = rig.data.filePath("applications/fugue-app-test-app.desktop");
        QTRY_VERIFY(QFileInfo::exists(entry));
        QFile file(entry);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.contains("Name=Block title\n"));
        QVERIFY(text.contains("Exec=fugue --app test-app\n"));
        QTRY_VERIFY(find(app.window, "appLauncherToggle")->property("current").toBool());
        QVERIFY(find(app.window, "appLauncherToggle")->property("tip").toString().startsWith("In the app launcher"));
        QCOMPARE(app.workflows.notice(), QStringLiteral("Added to the app launcher"));

        tap(app.window, find(app.window, "appLauncherToggle"));
        QTRY_VERIFY(!QFileInfo::exists(entry));
        QTRY_VERIFY(!find(app.window, "appLauncherToggle")->property("current").toBool());
        QVERIFY(find(app.window, "appLauncherToggle")->property("tip").toString().startsWith("Add to the app launcher"));

        // Open in Fugue: the full window, in a process of its own.
        tap(app.window, find(app.window, "appOpenInFugue"));
        QCOMPARE(rig.spawned.size(), 1);
        QCOMPARE(rig.spawned.first(), (QStringList{"--edit", "test-app"}));

        // The Get ready sheet of the full window is this window's too, and closing the window asks nothing.
        app.workflows.readiness()->show();
        QTRY_VERIFY(app.window->findChild<QObject *>("getReadySheet"));
        QCOMPARE(app.window->findChild<QObject *>("getReadySheet")->property("visible").toBool(), true);
        qDeleteAll(app.engine.rootObjects());
    }

    // FUGUE_SHOT_DIR=<folder>: the failed run's error line with its copy button, and the hero's launcher toggle off and on, over the
    // Dusk gradient (no preview) and over a pale preview picture. Pictures only; nothing is asserted.
    void theErrorCopyButtonAndTheLauncherToggleLook() {
        const QString out = qEnvironmentVariable("FUGUE_SHOT_DIR");
        if (out.isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to take the pictures");
        QDir().mkpath(out);
        for (const bool pale : {false, true}) {
            QTemporaryDir root;
            makeApp(root.path(), "shot-app", "Shot app");
            if (pale) {
                QImage picture(640, 400, QImage::Format_RGB32);
                for (int y = 0; y < picture.height(); ++y)
                    for (int x = 0; x < picture.width(); ++x)
                        picture.setPixelColor(x, y, QColor::fromHsv(40 + x * 40 / picture.width(), 25, 235 + 20 * y / picture.height()));
                QVERIFY(picture.save(root.path() + "/shot-app/preview.png"));
            }
            App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
            Rig rig(app, root.path());
            QVERIFY(app.workflows.openPath(root.path() + "/shot-app/workflow.fugue"));
            rig.view.showApp();
            QVERIFY(app.open());
            app.window->resize(1100, 700);
            QTRY_VERIFY(find(app.window, "appLauncherToggle"));
            const QString tag = pale ? "pale" : "dusk";
            const auto hero = [&](const char *state) {
                QTest::qWait(500); // the chip's fade and the measuring of the picture under it
                QImage whole = test_support::grab(app.window);
                QQuickItem *toggle = find(app.window, "appLauncherToggle");
                const QPoint at = toggle->mapToScene(QPointF(0, 0)).toPoint();
                whole.copy(QRect(at - QPoint(160, 40), QSize(260, 100))).scaled(780, 300, Qt::KeepAspectRatio, Qt::FastTransformation)
                    .save(QString("%1/launcher-%2-%3.png").arg(out, tag, state));
            };
            hero("off");
            tap(app.window, find(app.window, "appLauncherToggle"));
            QTRY_VERIFY(find(app.window, "appLauncherToggle")->property("current").toBool());
            tap(app.window, find(app.window, "appPage")); // off the toggle, so the on-state is not hover
            hero("on");
            if (pale)
                continue;
            rig.view.run();
            rig.view.onEvent("r1", QJsonObject{{"event", "run.done"}, {"status", "failed"},
                                               {"errors", QJsonArray{QJsonObject{{"code", "oom"}, {"message", "Not enough GPU memory (request abc123)."}, {"hint", "Close other programs using the GPU, or lower the size."}}}}});
            QTRY_VERIFY(find(app.window, "appErrorCopy"));
            QTest::qWait(300);
            test_support::grab(app.window).save(out + "/app-error-copy.png");
            qDeleteAll(app.engine.rootObjects());
        }
    }

    // `fugue --app x.fugueapp` (#394): the bundle opens as the app, and once it is ready "Keep this app" puts it in the person's
    // apps (their templates folder) and the launcher; the bundle itself is left where it was.
    void aBundleIsKeptIntoTheAppsAndTheLauncher() {
        QTemporaryDir root, out;
        makeApp(root.path(), "kept-app", "Kept app");
        const Result<Document> document = Document::loadFile(root.path() + "/kept-app/workflow.fugue");
        QVERIFY(document);
        ShareRequest request;
        request.title = "Kept app";
        request.app = true;
        request.workflowPath = root.path() + "/kept-app/workflow.fugue";
        request.appVersion = "1.0.0";
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        const QString bundle = out.filePath("kept.fuguepack"); // renamed: the reader goes by content
        QVERIFY(!hasErrors(writeSharePackage(*document, app.registry, request, bundle)));
        Rig rig(app, root.path());
        QVERIFY2(app.workflows.openPath(bundle), qPrintable(app.workflows.notice()));
        rig.view.showApp();
        const std::optional<AppRef> ref = resolveApp(bundle, {{root.path(), false}}, &app.registry);
        QVERIFY(ref && ref->kind == AppRef::Kind::Package);
        rig.launcher.setApp(*ref);
        QVERIFY(app.open());
        QTRY_VERIFY(find(app.window, "appKeep"));
        QCOMPARE(app.window->title(), QStringLiteral("Kept app"));
        tap(app.window, find(app.window, "appKeep"));
        const QString kept = TemplateLibrary::ownFolder() + "/kept-app";
        QVERIFY2(QFileInfo(kept + "/workflow.fugue").isFile(), qPrintable(app.workflows.notice()));
        QVERIFY(QFileInfo::exists(rig.data.filePath("applications/fugue-app-kept-app.desktop")));
        QTRY_VERIFY(!find(app.window, "appKeep")); // kept: the offer goes
        QVERIFY(rig.launcher.listed());
        QVERIFY(QFileInfo::exists(bundle));
        QVERIFY(app.workflows.notice().contains("kept"));
        QDir(kept).removeRecursively();
        qDeleteAll(app.engine.rootObjects());
    }

    // The rail's Apps: the shelf over the page; choosing another app switches this window to it in place (its tab, its title, its
    // launcher entry and icon), and there is no Graph item in this window.
    void theRailsAppsSwitchesTheWindowToAnotherAppInPlace() {
        QTemporaryDir root;
        makeApp(root.path(), "first", "First app");
        makeApp(root.path(), "second", "Second app");
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app, root.path());
        QVERIFY(app.workflows.openTemplateById("first"));
        rig.view.showApp();
        QVERIFY(app.open());
        QTRY_COMPARE(find(app.window, "appTitle")->property("title").toString(), QStringLiteral("First app"));
        QVERIFY(find(app.window, "appRail"));
        QVERIFY(!find(app.window, "appOpenInGraph")); // the app's own window has no graph to open
        QVERIFY(find(app.window, "appRailResults"));

        tap(app.window, find(app.window, "appRailApps"));
        QTRY_VERIFY(app.window->findChild<QObject *>("appsDialog"));
        QTRY_VERIFY(app.window->findChild<QObject *>("appsDialog")->property("visible").toBool());
        QTRY_COMPARE(rig.shelf.apps().size(), 2);
        QVERIFY(rig.shelf.choose("second"));
        QTRY_COMPARE(find(app.window, "appTitle")->property("title").toString(), QStringLiteral("Second app"));
        QCOMPARE(app.window->title(), QStringLiteral("Second app"));
        QVERIFY(find(app.window, "appPage")); // still the App view, not a graph
        QVERIFY(!find(app.window, "canvas"));
        QCOMPARE(rig.icons, QStringList{"second"}); // the window's icon follows
        // The launcher entry and Open in Fugue are the new app's (the dialog the card would have closed is closed first).
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!app.window->findChild<QObject *>("appsDialog")->property("visible").toBool());
        if (const QByteArray dir = qgetenv("FUGUE_SHOT_DIR"); !dir.isEmpty()) { // FUGUE_SHOT_DIR=<dir>: a picture of the app's window
            QTest::qWait(500);
            test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/app-own-window.png");
        }
        QTRY_VERIFY(find(app.window, "appOpenInFugue"));
        tap(app.window, find(app.window, "appOpenInFugue"));
        QTRY_VERIFY(!rig.spawned.isEmpty());
        QCOMPARE(rig.spawned.last(), (QStringList{"--edit", "second"}));
        qDeleteAll(app.engine.rootObjects());
    }

    void aWorkflowWithoutABlockSaysSoAndOffersTheFullWindow() {
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app);
        QVERIFY(app.open());
        QTRY_VERIFY(find(app.window, "appNotAnApp"));
        QVERIFY(!find(app.window, "appPage"));
        QVERIFY(!item(app.window->contentItem(), "canvas"));
        qDeleteAll(app.engine.rootObjects());
    }
};

QTEST_MAIN(TestAppOwnWindow)
#include "test_app_own_window.moc"
