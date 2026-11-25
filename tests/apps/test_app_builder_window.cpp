// The app builder in the real window (#393, Main.qml, offscreen): Make app… opens the dialog, and the keyboard alone makes an app
// (Tab through the fields, the lists add inputs and outputs, Alt+Up moves a row, Enter in a field applies, Escape cancels); the
// click-to-expose mode takes a click on an input row of the canvas and ends with Escape; Apply and open shows the App view. The
// store's rules are test_app_builder. With FUGUE_BUILDER_SHOTS=<dir> (and FUGUE_SHOT_THEME, FUGUE_SHOT_LOOK="preset=flat",
// FUGUE_SHOT_TAG) a last test saves the builder, a canvas in click mode and the Edit app dialog as pictures.

#include <QDir>
#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"
#include "../window_grab.h"
#include "apps/app_builder.h"
#include "apps/app_shelf.h"
#include "apps/app_window.h"
#include "engine/app_binding.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

using namespace canvas_test;
using namespace fugue;

namespace {

const char *workflow = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "inputs/sample.png"}},
    "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "an old family portrait"}},
    "sample": {"type": "fugue/sampler", "version": "1.2.0", "inputs": {"seed": 3, "steps": 3, "conditioning": {"from": "encode.conditioning"}}},
    "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out"}}
  }},
  "layout": {"nodes": {"load": {"pos": [0, 0]}, "encode": {"pos": [420, 0]}, "sample": {"pos": [420, 320]}, "save": {"pos": [860, 0]}}}
})";

QQuickItem *visibleItem(QQuickItem *from, const QString &name) {
    for (QQuickItem *child : from->childItems()) {
        if (!child->isVisible())
            continue;
        if (child->objectName() == name)
            return child;
        if (QQuickItem *found = visibleItem(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem *find(QQuickWindow *window, const QString &name) {
    QQuickItem *root = window->contentItem();
    while (root->parentItem())
        root = root->parentItem();
    return visibleItem(root, name);
}

QPoint centerOf(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

// The runs and the view tied to a window as main.cpp ties them; a run is never started.
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
          view([](const RunRequest &) { return QStringLiteral("r1"); }, [](const QString &) {}, &app.registry),
          shelf([&app] { return app.workflows.templates(); }, [&app](const QString &id) { return app.workflows.openTemplateById(id); }) {
        app.appShelf = &shelf;
        runs.registerCommands(app.commands);
        app.runs = &runs;
        bindAppView(view, app.workflows, app.model, service, app.commands, runs);
        app.appView = &view;
    }
};

} // namespace

class TestAppBuilderWindow : public QObject {
    Q_OBJECT

    bool dialogOpen(App &app) const {
        const QObject *box = app.window->findChild<QObject *>("appBuilderDialog");
        return box && box->property("opened").toBool();
    }
    // Tab until the focus is in the item of that name (or inside it).
    bool tabTo(App &app, const QString &name, int limit = 60) const {
        for (int i = 0; i < limit; ++i) {
            for (QQuickItem *at = app.window->activeFocusItem(); at; at = at->parentItem())
                if (at->objectName() == name)
                    return true;
            QTest::keyClick(app.window, Qt::Key_Tab);
        }
        return false;
    }
    void click(App &app, const QString &name) {
        QPoint at;
        int still = 0;
        for (int i = 0; still < 5 && i < 500; ++i) {
            QTest::qWait(10);
            QQuickItem *now = find(app.window, name);
            if (!now) {
                still = 0;
                continue;
            }
            const QPoint here = centerOf(now);
            still = here == at ? still + 1 : 0;
            at = here;
        }
        QVERIFY2(still >= 3, qPrintable(QStringLiteral("%1 never settled").arg(name)));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, at);
    }
    QJsonValue block(App &app) const { return app.model.document().extra.value(QStringLiteral("app")); }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theKeyboardAloneMakesAnApp() {
        App app(load(workflow));
        Rig rig(app);
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("app.make"));
        QTRY_VERIFY(dialogOpen(app));
        QVERIFY(find(app.window, "builderTitle"));
        // The title field has the focus; the workflow's name is its start.
        for (int i = 0; i < 40 && !app.model.appBuilder()->title().isEmpty(); ++i)
            QTest::keyClick(app.window, Qt::Key_Backspace);
        for (const char letter : QByteArray("Restyle"))
            QTest::keyClick(app.window, letter);
        QCOMPARE(app.model.appBuilder()->title(), QStringLiteral("Restyle"));

        // Add an input and an output from the lists: the combo box opens with Down and is chosen with Enter.
        QVERIFY(tabTo(app, "builderAddInput"));
        const auto choose = [&app](int downs) {
            QTest::keyClick(app.window, Qt::Key_Space); // opens the list
            for (int i = 0; i < downs; ++i)
                QTest::keyClick(app.window, Qt::Key_Down);
            QTest::keyClick(app.window, Qt::Key_Return);
        };
        // The list is sorted by node title; the first rows are the inputs of Load Image and the next ones the encoder's.
        choose(1);
        QTRY_COMPARE(app.model.appBuilder()->inputs().size(), 1);
        QVERIFY(tabTo(app, "builderAddInput"));
        choose(3);
        QTRY_COMPARE(app.model.appBuilder()->inputs().size(), 2);
        QVERIFY(tabTo(app, "builderAddOutput"));
        choose(1);
        QTRY_COMPARE(app.model.appBuilder()->outputs().size(), 1);

        // Alt+Up in a row's label moves the row up.
        const QString second = app.model.appBuilder()->inputs().at(1).toMap().value("key").toString();
        QVERIFY(tabTo(app, "builderLabel-1"));
        QTest::keyClick(app.window, Qt::Key_Up, Qt::AltModifier);
        QTRY_COMPARE(app.model.appBuilder()->inputs().first().toMap().value("key").toString(), second);

        // Enter in a field applies.
        QVERIFY(tabTo(app, "builderTitle", 80));
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(!dialogOpen(app));
        QVERIFY(block(app).isObject());
        QCOMPARE(block(app).toObject().value("title").toString(), QStringLiteral("Restyle"));
        QCOMPARE(app.model.undoStack()->count(), 1);
        app.model.undo();
        QVERIFY(!block(app).isObject());
        qDeleteAll(app.engine.rootObjects());
    }

    void escapeCancelsAndLeavesTheWorkflowAlone() {
        App app(load(workflow));
        Rig rig(app);
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("app.make"));
        QTRY_VERIFY(dialogOpen(app));
        QVERIFY(app.model.appBuilder()->exposeInput("encode", "prompt"));
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialogOpen(app));
        QVERIFY(!app.model.appBuilder()->isOpen());
        QVERIFY(!block(app).isObject());
        QCOMPARE(app.model.undoStack()->count(), 0);
        qDeleteAll(app.engine.rootObjects());
    }

    void aClickOnTheCanvasPicksAnInputAndEscapeBringsTheDialogBack() {
        App app(load(workflow));
        Rig rig(app);
        QVERIFY(app.open());
        app.window->resize(1600, 900);
        QVERIFY(app.commands.trigger("view.fit")); // the whole graph is on screen, so a click can reach any node
        QTest::qWait(300);
        QVERIFY(app.commands.trigger("app.make"));
        QTRY_VERIFY(dialogOpen(app));
        click(app, "builderPick");
        QTRY_VERIFY(!dialogOpen(app));
        QVERIFY(app.model.appBuilder()->picking());
        QTRY_VERIFY(find(app.window, "appPickBanner"));
        QTRY_VERIFY(find(app.window, "appPickLayer")); // made only while picking
        const QRectF row = app.model.inputRowRect("encode", "prompt");
        QVERIFY(!row.isEmpty());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(row.center().x(), row.center().y()));
        QTRY_COMPARE(app.model.appBuilder()->inputs().size(), 1);
        const QRectF sink = app.model.nodeRect("save");
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(sink.center().x(), sink.top() + 2));
        QTRY_COMPARE(app.model.appBuilder()->outputs().size(), 1);
        QCOMPARE(app.model.undoStack()->count(), 0); // picking is the draft's: the workflow is not touched
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!app.model.appBuilder()->picking());
        QTRY_VERIFY(!find(app.window, "appPickLayer") && !find(app.window, "appPickBanner")); // and gone after
        QTRY_VERIFY(dialogOpen(app));
        QVERIFY(app.model.appBuilder()->isOpen());
        QCOMPARE(app.model.appBuilder()->inputs().size(), 1);
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialogOpen(app));
        qDeleteAll(app.engine.rootObjects());
    }

    void plainApplyStaysInTheGraph() {
        App app(load(workflow));
        Rig rig(app);
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("app.make"));
        QTRY_VERIFY(dialogOpen(app));
        AppBuilder *builder = app.model.appBuilder();
        builder->setTitle("Restyle");
        QVERIFY(builder->exposeInput("encode", "prompt"));
        QVERIFY(builder->addOutput("save"));
        click(app, "builderApply");
        QTRY_VERIFY(!dialogOpen(app));
        QVERIFY(block(app).isObject());
        QVERIFY(rig.view.isApp());
        QVERIFY(!rig.view.shown()); // the graph stays; the App view is one keystroke (view.toggle-app) away
        QVERIFY(find(app.window, "canvas"));
        QVERIFY(app.commands.trigger("view.toggle-app"));
        QTRY_VERIFY(find(app.window, "appPage"));
        qDeleteAll(app.engine.rootObjects());
    }

    void applyAndOpenShowsTheAppView() {
        App app(load(workflow));
        Rig rig(app);
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("app.make"));
        QTRY_VERIFY(dialogOpen(app));
        AppBuilder *builder = app.model.appBuilder();
        builder->setTitle("Restyle");
        QVERIFY(builder->exposeInput("encode", "prompt"));
        QVERIFY(builder->exposeInput("sample", "steps"));
        QVERIFY(builder->addOutput("save"));
        QTRY_VERIFY(find(app.window, "builderPreview"));
        click(app, "builderApplyOpen");
        QTRY_VERIFY(!dialogOpen(app));
        QVERIFY(block(app).isObject());
        QTRY_VERIFY(find(app.window, "appPage"));
        QVERIFY(find(app.window, "appInput-prompt"));
        QVERIFY(rig.view.shown());
        // Edit app is the same dialog, on the block.
        QVERIFY(app.commands.trigger("app.edit"));
        QTRY_VERIFY(dialogOpen(app));
        QVERIFY(builder->editing());
        QCOMPARE(builder->inputs().size(), 2);
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialogOpen(app));
        app.model.undo();
        QVERIFY(!block(app).isObject());
        qDeleteAll(app.engine.rootObjects());
    }

    // FUGUE_BUILDER_SHOTS=<dir> [FUGUE_SHOT_THEME=<omarchy theme>] [FUGUE_SHOT_LOOK="preset=flat"] [FUGUE_SHOT_TAG=<tag>]
    void screenshots() {
        const QString out = qEnvironmentVariable("FUGUE_BUILDER_SHOTS");
        if (out.isEmpty())
            QSKIP("Set FUGUE_BUILDER_SHOTS to a folder to write the pictures there.");
        QDir().mkpath(out);
        const QString tag = qEnvironmentVariable("FUGUE_SHOT_TAG", QStringLiteral("shot"));
        App app(load(workflow), {}, Look{qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("tokyo-night")), 12});
        Rig rig(app);
        QVERIFY(app.open());
        for (const QString &part : qEnvironmentVariable("FUGUE_SHOT_LOOK").split(';', Qt::SkipEmptyParts)) {
            const QStringList kv = part.split('=');
            QVERIFY(kv.size() == 2);
            QCOMPARE(kv[0] == "preset" ? app.settings.applyLookPreset(kv[1]) : app.settings.set(kv[0], kv[1].toDouble()), QString());
        }
        app.window->resize(1888, 971);
        QTest::qWait(300);
        const auto save = [&](const QString &what) {
            QTest::qWait(450);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/%2-%3.png").arg(out, tag, what)));
        };
        QTest::mouseMove(app.window, QPoint(40, 600));
        QVERIFY(app.commands.trigger("app.make"));
        QTRY_VERIFY(dialogOpen(app));
        AppBuilder *builder = app.model.appBuilder();
        builder->setTitle("Restyle a portrait");
        builder->setDescription("Paints a picture in the style you describe.");
        QVERIFY(builder->exposeInput("load", "file"));
        QVERIFY(builder->exposeInput("encode", "prompt"));
        QVERIFY(builder->editInput("prompt", {{"label", "What is in the picture"}}));
        QVERIFY(builder->exposeInput("sample", "steps"));
        QVERIFY(builder->editInput("steps", {{"label", "Passes"}, {"min", "1"}, {"max", "8"}}));
        QVERIFY(builder->exposeInput("sample", "seed"));
        QVERIFY(builder->editInput("seed", {{"advanced", true}}));
        QVERIFY(builder->addOutput("save"));
        QVERIFY(builder->setOutputLabel("save", "Picture"));
        save("builder");
        click(app, "builderPick");
        QTRY_VERIFY(!dialogOpen(app));
        QTest::mouseMove(app.window, app.screen(app.model.inputRowRect("sample", "steps").center().x(), app.model.inputRowRect("sample", "steps").center().y()));
        save("picking");
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(dialogOpen(app));
        click(app, "builderApply");
        QTRY_VERIFY(!dialogOpen(app));
        QVERIFY(app.commands.trigger("app.edit"));
        QTRY_VERIFY(dialogOpen(app));
        save("edit");
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialogOpen(app));
        qDeleteAll(app.engine.rootObjects());
    }
};

QTEST_MAIN(TestAppBuilderWindow)
#include "test_app_builder_window.moc"
