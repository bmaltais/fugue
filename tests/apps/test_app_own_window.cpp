// An app as its own application (#398, apps/AppWindow.qml, offscreen, QTest events only): the window is the App view and nothing
// else, its title is the app's, Add to launcher and Remove from launcher write and take away an entry in a scratch data folder,
// Open in Fugue asks for the full window, and the Get ready sheet of #379 opens inside it. The entry's file is
// test_launcher_entries; the store's logic is test_app_view.

#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"
#include "apps/app_launcher.h"
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

void tap(QQuickWindow *window, QQuickItem *item) {
    QTest::mouseClick(window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
}

const QString templates = QStringLiteral(FUGUE_SOURCE_DIR "/tests/apps/templates");

// What main() ties for an app's window: the runs (never started), the view and the launcher over a scratch data folder.
struct Rig {
    RunSetup setup;
    RunService service;
    CanvasRuns runs;
    AppView view;
    QTemporaryDir data;
    QList<QStringList> spawned;
    AppLauncher launcher;

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
          view([](const RunRequest &) { return QStringLiteral("r1"); }, [](const QString &) {}, &app.registry),
          launcher(&view, &app.workflows, {{templates, false}}, &app.registry, LauncherEntries(data.path(), "fugue"),
                   [this](const QStringList &arguments) { spawned << arguments; return true; }) {
        app.workflows.setTemplateRoots({{templates, false}});
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
        for (const char *chrome : {"menuBar", "canvas", "areas", "areaHost", "appShowApp"})
            QVERIFY2(!item(app.window->contentItem(), chrome), chrome);
        QVERIFY(!find(app.window, "appOpenInGraph")); // no graph to open
        QCOMPARE(app.window->title(), QStringLiteral("Block title")); // the app's own
        QVERIFY(find(app.window, "appInput-prompt"));
        QVERIFY(find(app.window, "appRun"));

        // Add to launcher: an entry that starts this app, in the scratch folder only.
        QTRY_VERIFY(find(app.window, "appLauncherToggle"));
        QQuickItem *toggle = find(app.window, "appLauncherToggle");
        QCOMPARE(toggle->property("text").toString(), QStringLiteral("Add to launcher"));
        tap(app.window, toggle);
        const QString entry = rig.data.filePath("applications/fugue-app-test-app.desktop");
        QTRY_VERIFY(QFileInfo::exists(entry));
        QFile file(entry);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.contains("Name=Block title\n"));
        QVERIFY(text.contains("Exec=fugue --app test-app\n"));
        QTRY_COMPARE(find(app.window, "appLauncherToggle")->property("text").toString(), QStringLiteral("Remove from launcher"));
        QVERIFY(app.workflows.notice().contains("launcher"));

        tap(app.window, find(app.window, "appLauncherToggle"));
        QTRY_VERIFY(!QFileInfo::exists(entry));
        QTRY_COMPARE(find(app.window, "appLauncherToggle")->property("text").toString(), QStringLiteral("Add to launcher"));

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
