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


void saveGrab(QQuickWindow *window, const QString &name) {
    const QString out = qEnvironmentVariable("FUGUE_GRAB_DIR");
    if (out.isEmpty())
        return;
    QDir().mkpath(out);
    test_support::grab(window).save(out + "/" + name + ".png");
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
        shelf.setBookmarksFile(app.dir.path() + "/app-bookmarks.json");
        shelf.setActions([&app](const QString &id, const QString &title) { return app.workflows.duplicateTemplate(id, title); },
                         [&app](const QString &id, const QString &title) { return app.workflows.renameTemplate(id, title); });
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
        QVERIFY(!find(app.window, "appViewSwitch")); // the shelf is no app: nothing to switch
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
        QCOMPARE(find(app.window, "appTitle")->property("title").toString(), QStringLiteral("Ready app"));
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

        // Fugue's mark at the start of the window bar is the same command, in the graph too: it opens the shelf, Escape closes it.
        QVERIFY(find(app.window, "canvas"));
        tap(app.window, "fugueMark");
        QTRY_VERIFY(find(app.window, "appCard-ready-app"));
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!app.window->findChild<QObject *>("appsDialog")->property("visible").toBool());

        QVERIFY(app.commands.run("app.show-all"));
        QTRY_VERIFY(find(app.window, "appCard-ready-app"));
        QVERIFY(app.window->findChild<QObject *>("appsDialog")->property("visible").toBool());
        tap(app.window, "appCard-ready-app");
        QTRY_VERIFY(find(app.window, "appPage"));
        QTRY_VERIFY(!app.window->findChild<QObject *>("appsDialog")->property("visible").toBool());
        qDeleteAll(app.engine.rootObjects());
    }

    // Open in Graph from the shelf's app gives the whole window back (the owner's report on #397): the canvas fitted to the
    // workflow's nodes, the window bar with its menus and tabs, no App page and no shelf.
    void openInGraphFromTheShelfShowsTheWholeWindow() {
        QTemporaryDir root;
        makeApp(root.path(), "ready-app", "Ready app", false);
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app);
        app.workflows.setTemplateRoots({{root.path(), false}});
        QVERIFY(app.open());
        rig.shelf.setLanding(app.workflows.nothingOpen());
        QTRY_VERIFY(find(app.window, "appCard-ready-app"));
        app.window->resize(1500, 900); // the window is tiled to its place while the shelf hides the canvas, as on Hyprland
        QTRY_COMPARE(app.window->width(), 1500);
        tap(app.window, "appCard-ready-app");
        QTRY_VERIFY(find(app.window, "appPage"));
        saveGrab(app.window, "app-from-shelf");
        rig.view.openInGraph();
        QTRY_VERIFY(find(app.window, "canvas"));
        QTRY_VERIFY(!find(app.window, "appPage"));
        QVERIFY(!find(app.window, "appsHome"));
        QTRY_VERIFY(find(app.window, "menuBar"));
        QVERIFY(find(app.window, "appViewSwitch")); // the way back to the App view
        QVERIFY(find(app.window, "areaHeader-1")); // the graph's own menus
        QQuickItem *canvas = find(app.window, "canvas");
        QTRY_VERIFY(canvas->width() > 1300); // laid out at the resized window's width
        QTRY_VERIFY(app.model.view().contains("zoom"));
        const double zoom = app.model.view().value("zoom").toDouble();
        QVERIFY2(zoom > 0.05 && zoom <= 2.0, qPrintable(QString::number(zoom)));
        // The nodes are in the middle of the canvas as it is now, not of the size it had before the window was resized: the point
        // in the canvas's middle is the view's center.
        const auto middleIsCenter = [&] {
            QVariant middle;
            if (!QMetaObject::invokeMethod(canvas, "toWorld", Q_RETURN_ARG(QVariant, middle), Q_ARG(QVariant, canvas->width() / 2),
                                           Q_ARG(QVariant, canvas->height() / 2)))
                return false;
            const QVariantList center = app.model.view().value("center").toList();
            return qAbs(middle.toPointF().x() - center.at(0).toDouble()) < 1.5 && qAbs(middle.toPointF().y() - center.at(1).toDouble()) < 1.5;
        };
        QTRY_VERIFY(middleIsCenter());
        // Fitted to the canvas as it is now: fitting again here changes nothing.
        const QVariantMap before = app.model.view();
        QVERIFY(app.model.fitView(canvas->width(), canvas->height(), canvas->property("reservedTop").toReal(), canvas->property("reservedBottom").toReal()));
        QCOMPARE(app.model.view().value("zoom").toDouble(), before.value("zoom").toDouble());
        QCOMPARE(app.model.view().value("center").toList(), before.value("center").toList());
        saveGrab(app.window, "graph-from-shelf");
        qDeleteAll(app.engine.rootObjects());
    }


    // The shelf by keys alone (#463, #470): the focus is on the search, which narrows the cards live; Down goes to the cards,
    // Ctrl+B pins one to the top (kept in a file), F2 renames one of the person's own apps, Ctrl+D copies one and opens the copy in the
    // App view. A bundled app has no Rename.
    void theShelfSearchesPinsRenamesAndDuplicatesByKeys() {
        QTemporaryDir root, mine;
        makeApp(root.path(), "alpha-app", "Alpha app", false);
        makeApp(root.path(), "beta-app", "Beta app", false);
        makeApp(mine.path(), "own-app", "Own app", false);
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app);
        app.workflows.setTemplateRoots({{mine.path(), true}, {root.path(), false}});
        QVERIFY(app.open());
        rig.shelf.setLanding(app.workflows.nothingOpen());
        QQuickItem *search = nullptr;
        QTRY_VERIFY((search = find(app.window, "appsSearch")) && search->hasActiveFocus());
        QQuickItem *grid = nullptr;
        QTRY_VERIFY((grid = find(app.window, "appShelfGrid")));
        const auto order = [&] {
            QStringList ids;
            for (const QVariant &card : rig.shelf.shown())
                ids << card.toMap().value("id").toString();
            return ids;
        };
        QTRY_COMPARE(order(), (QStringList{"own-app", "alpha-app", "beta-app"}));

        // Typing narrows the cards at once; the others have no card.
        for (const char c : {'b', 'e', 't'})
            QTest::keyClick(app.window, c);
        QTRY_COMPARE(order(), QStringList{"beta-app"});
        QTRY_VERIFY(!find(app.window, "appCard-alpha-app"));
        QVERIFY(find(app.window, "appCard-beta-app"));
        search->setProperty("text", "nothing like it");
        QTRY_VERIFY(find(app.window, "appShelfEmpty"));
        QVERIFY(find(app.window, "appShelfEmptyText")->property("text").toString().contains("nothing like it"));
        search->setProperty("text", "");
        QTRY_COMPARE(order().size(), 3);

        // Down goes to the cards; Ctrl+B pins the focused one to the top, and it stays there in the next session.
        QTest::keyClick(app.window, Qt::Key_Down);
        QTRY_VERIFY(grid->hasActiveFocus());
        grid->setProperty("currentIndex", 2); // beta
        QTest::keyClick(app.window, Qt::Key_B, Qt::ControlModifier);
        QTRY_COMPARE(order().first(), QString("beta-app"));
        QVERIFY(rig.shelf.isBookmarked("beta-app"));
        QVERIFY(QFileInfo(app.dir.path() + "/app-bookmarks.json").size() > 0);

        // F2 on a bundled app does nothing; on the person's own it opens the name field, Enter keeps the new name.
        grid->setProperty("currentIndex", 2); // alpha, bundled
        QTest::keyClick(app.window, Qt::Key_F2);
        QTest::qWait(50);
        QVERIFY(!find(app.window, "appRename-alpha-app"));
        grid->setProperty("currentIndex", 1); // own-app
        QTest::keyClick(app.window, Qt::Key_F2);
        QQuickItem *name = nullptr;
        QTRY_VERIFY2((name = find(app.window, "appRename-own-app")), "the name field is made");
        QTRY_VERIFY2(name->hasActiveFocus(), "the name field has the keyboard");
        QCOMPARE(name->property("text").toString(), QString("Own app"));
        name->setProperty("text", "Renamed app");
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_COMPARE(find(app.window, "appTitle-own-app")->property("text").toString(), QString("Renamed app"));
        QVERIFY(!find(app.window, "appRename-own-app"));
        QTRY_VERIFY(grid->hasActiveFocus());
        // Escape in the name field keeps the old name.
        QTest::keyClick(app.window, Qt::Key_F2);
        QTRY_VERIFY((name = find(app.window, "appRename-own-app")) && name->hasActiveFocus());
        name->setProperty("text", "Never kept");
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!find(app.window, "appRename-own-app"));
        QCOMPARE(find(app.window, "appTitle-own-app")->property("text").toString(), QString("Renamed app"));

        // Ctrl+D makes the person's copy and opens it in the App view; the shelf goes.
        QTest::keyClick(app.window, Qt::Key_D, Qt::ControlModifier);
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(QFileInfo(mine.path() + "/own-app-copy/workflow.fugue").isFile());
        QVERIFY(!find(app.window, "appsHome"));
        qDeleteAll(app.engine.rootObjects());
    }


    // FUGUE_SHOT_DIR=<dir>: the shelf at home, the Apps dialog over a graph and the Templates window, at 1888x971 and 900x700, over
    // the bundled templates (their moving previews stay on the poster here: the clip needs the window to be the active one).
    // FUGUE_SHOT_THEME (default tokyo-night) and FUGUE_SHOT_LOOK ("preset=flat;frost=0") as test_app_window's. For looking.
    void theShelfAndTemplatesInTwoSizes() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to take the pictures");
        const QString themeName = qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("tokyo-night"));
        const QString tag = qEnvironmentVariable("FUGUE_SHOT_TAG");
        const QString out = QString::fromLocal8Bit(dir) + "/" + themeName + (tag.isEmpty() ? QString() : "-" + tag);
        QDir().mkpath(out);
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"), {}, Look{themeName, 12});
        Rig rig(app);
        app.workflows.setTemplateRoots({{QStringLiteral(FUGUE_SOURCE_DIR "/templates"), false}});
        QVERIFY(app.open());
        for (const QString &part : qEnvironmentVariable("FUGUE_SHOT_LOOK").split(';', Qt::SkipEmptyParts)) {
            const QStringList kv = part.split('=');
            QVERIFY(kv.size() == 2);
            QCOMPARE(kv[0] == "preset" ? app.settings.applyLookPreset(kv[1]) : app.settings.set(kv[0], kv[1].toDouble()), QString());
        }
        rig.shelf.toggleBookmark("remove-background"); // one pinned, to see it
        rig.shelf.setLanding(app.workflows.nothingOpen());
        const auto grab = [&](const QString &name) {
            QTest::qWait(400); // the pictures decode, the frosted buttons measure
            test_support::grab(app.window).save(out + "/" + name + ".png");
        };
        const QList<QSize> sizes{QSize(1888, 971), QSize(900, 700)};
        const auto suffixOf = [](const QSize &size) { return QString("-%1x%2").arg(size.width()).arg(size.height()); };
        for (const QSize &size : sizes) {
            app.window->resize(size);
            QTRY_COMPARE(app.window->width(), size.width());
            QTRY_VERIFY(find(app.window, "appsHome"));
            QTRY_VERIFY(find(app.window, "appCard-remove-background"));
            grab("shelf-home" + suffixOf(size));
            if (QQuickItem *card = find(app.window, "appCard-image-upscale")) { // the pointer on a card: its buttons show
                QTest::mouseMove(app.window, card->mapToScene(QPointF(card->width() / 2, card->height() / 3)).toPoint());
                grab("shelf-hover" + suffixOf(size));
            }
            QTest::mouseMove(app.window, QPoint(2, 2));
        }
        tap(app.window, "appsStartBlank");
        QTRY_VERIFY(find(app.window, "canvas"));
        for (const QSize &size : sizes) {
            app.window->resize(size);
            QTRY_COMPARE(app.window->width(), size.width());
            QVERIFY(app.commands.run("app.show-all"));
            QTRY_VERIFY(app.window->findChild<QObject *>("appsDialog")->property("opened").toBool());
            grab("apps-dialog" + suffixOf(size));
            QTest::keyClick(app.window, Qt::Key_Escape);
            QTRY_VERIFY(!app.window->findChild<QObject *>("appsDialog")->property("visible").toBool());
            QVERIFY(app.commands.run("file.templates"));
            QTRY_VERIFY(app.window->findChild<QObject *>("templatesDialog")->property("opened").toBool());
            QTRY_VERIFY(find(app.window, "templatesGrid"));
            grab("templates" + suffixOf(size));
            QTest::keyClick(app.window, Qt::Key_Escape);
            QTRY_VERIFY(!app.window->findChild<QObject *>("templatesDialog")->property("visible").toBool());
        }
        qDeleteAll(app.engine.rootObjects());
    }


    // Every control of the shelf is reachable by keys (the epic's keyboard rule): Shift+Tab from the search goes back to the header's
    // buttons, Enter on Start blank gives the graph.
    void theHeaderButtonsAreReachableByTab_data() {
        QTest::addColumn<QString>("preset");
        QTest::newRow("calm") << "calm";
        QTest::newRow("flat") << "flat";
    }
    void theHeaderButtonsAreReachableByTab() {
        QFETCH(QString, preset);
        QTemporaryDir root;
        makeApp(root.path(), "alpha-app", "Alpha app", false);
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app);
        app.workflows.setTemplateRoots({{root.path(), false}});
        QVERIFY(app.open());
        QCOMPARE(app.settings.applyLookPreset(preset), QString());
        rig.shelf.setLanding(app.workflows.nothingOpen());
        QQuickItem *search = nullptr;
        QTRY_VERIFY((search = find(app.window, "appsSearch")) && search->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Backtab);
        QTRY_VERIFY2(app.window->activeFocusItem() && app.window->activeFocusItem()->objectName() == "appsStartBlank",
                     qPrintable(app.window->activeFocusItem() ? app.window->activeFocusItem()->objectName() : "none"));
        QTest::keyClick(app.window, Qt::Key_Backtab);
        QTRY_COMPARE(app.window->activeFocusItem()->objectName(), QString("appsBrowseTemplates"));
        QTest::keyClick(app.window, Qt::Key_Tab);
        QTRY_COMPARE(app.window->activeFocusItem()->objectName(), QString("appsStartBlank"));
        QTest::keyClick(app.window, Qt::Key_Tab); // round: the search, the cards, the header again
        QTRY_VERIFY(search->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Tab);
        QTRY_VERIFY(find(app.window, "appShelfGrid")->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Tab);
        QTRY_COMPARE(app.window->activeFocusItem()->objectName(), QString("appsBrowseTemplates"));
        QTest::keyClick(app.window, Qt::Key_Tab);
        QTRY_COMPARE(app.window->activeFocusItem()->objectName(), QString("appsStartBlank"));
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(find(app.window, "canvas"));
        QTRY_VERIFY(!find(app.window, "appsHome"));
        qDeleteAll(app.engine.rootObjects());
    }

    // The performance budget (docs/canvas-look.md 20.17, #467): a settled shelf swaps no frame, in the Calm and the Flat look (the moving
    // previews play only when hovered or focused; none is here).
    void anIdleShelfDoesNotRedraw_data() {
        QTest::addColumn<QString>("preset");
        QTest::newRow("calm") << "calm";
        QTest::newRow("flat") << "flat";
    }
    void anIdleShelfDoesNotRedraw() {
        QFETCH(QString, preset);
        QTemporaryDir root;
        makeApp(root.path(), "alpha-app", "Alpha app", false);
        makeApp(root.path(), "beta-app", "Beta app", true);
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app);
        app.workflows.setTemplateRoots({{root.path(), false}});
        QElapsedTimer opened;
        opened.start();
        QVERIFY(app.open());
        QCOMPARE(app.settings.applyLookPreset(preset), QString());
        rig.shelf.setLanding(app.workflows.nothingOpen());
        QTRY_VERIFY(find(app.window, "appShelfGrid"));
        qInfo() << "the shelf's first frame after open," << preset << ":" << opened.elapsed() << "ms";
        find(app.window, "appShelfGrid")->forceActiveFocus(); // the search field's blinking cursor is a frame of its own
        QTest::mouseMove(app.window, QPoint(5, 5));
        int frames = 0;
        const auto connection = connect(app.window, &QQuickWindow::frameSwapped, this, [&frames] { ++frames; });
        bool quiet = false;
        for (int i = 0; i < 20 && !quiet; ++i) {
            frames = 0;
            QTest::qWait(500);
            quiet = frames == 0;
        }
        QVERIFY2(quiet, "the shelf never stopped drawing");
        QTest::qWait(1000);
        disconnect(connection);
        QCOMPARE(frames, 0);
        qDeleteAll(app.engine.rootObjects());
    }

    // The Templates window by keys alone (the epic's keyboard rule, #467): it opens with the search focused, Down enters the grid, Enter
    // opens the card's template, Escape closes it and gives the focus back to what opened it.
    void theTemplatesWindowIsKeyboardOnly() {
        QTemporaryDir root;
        makeApp(root.path(), "alpha-app", "Alpha app", false);
        App app(load("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        Rig rig(app);
        app.workflows.setTemplateRoots({{root.path(), false}});
        QVERIFY(app.open());
        rig.shelf.setLanding(app.workflows.nothingOpen());
        QQuickItem *shelfSearch = nullptr;
        QTRY_VERIFY((shelfSearch = find(app.window, "appsSearch")) && shelfSearch->hasActiveFocus());
        QVERIFY(app.commands.run("file.templates"));
        QTRY_VERIFY(app.window->findChild<QObject *>("templatesDialog")->property("opened").toBool());
        QQuickItem *search = nullptr;
        QTRY_VERIFY((search = find(app.window, "templatesSearch")) && search->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Down);
        QQuickItem *grid = find(app.window, "templatesGrid");
        QVERIFY(grid);
        QTRY_VERIFY(grid->hasActiveFocus());
        QCOMPARE(grid->property("currentIndex").toInt(), 0);
        QTest::keyClick(app.window, Qt::Key_Tab); // out of the grid, on to the next control, never lost
        QTRY_VERIFY(app.window->activeFocusItem() && app.window->activeFocusItem() != grid);
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!app.window->findChild<QObject *>("templatesDialog")->property("visible").toBool());
        QTRY_VERIFY(shelfSearch->hasActiveFocus());
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
