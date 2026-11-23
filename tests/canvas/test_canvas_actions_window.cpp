// The Canvas view's task bar in the real window (#231, docs/canvas-actions-design.md 4, 12), offscreen, QTest events
// only (no real window, no real pointer), with the window's RunService and a FakeProvider, the canvas store in the
// test's own FUGUE_HOME: draw a rectangle and the bar floats under it; choose an action, type a prompt, Enter, and a
// real queued run shows its progress and the shimmer, then lands a layer the layers panel lists; Escape cancels; an
// error is one plain line; an action whose models are missing offers Download and keeps Generate off; the same in a
// Canvas area. FUGUE_SHOT_DIR pictures, dark and light, and a narrow area.

#include <QDir>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas_actions_rig.h"
#include "../window_grab.h"
#include "engine/canvas_store.h"

using namespace actions_test;

namespace {

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

QPoint settled(QQuickWindow *window, const QString &name) {
    QPoint at(-1, -1);
    int still = 0;
    for (int i = 0; still < 3 && i < 500; ++i) {
        QTest::qWait(20);
        QQuickItem *now = find(window, name);
        if (!now) {
            still = 0;
            continue;
        }
        const QPoint here = centerOf(now);
        still = here == at ? still + 1 : 0;
        at = here;
    }
    return still >= 3 ? at : QPoint(-1, -1);
}

// Types `text` into the window's focus, one key each (QTest::keyClicks is for widgets).
void typeInto(QQuickWindow *window, const QString &text) {
    for (const QChar c : text)
        QTest::keyClick(window, c.toLatin1());
}

QString textOf(QQuickWindow *window, const QString &name) {
    QQuickItem *item = find(window, name);
    return item ? item->property("text").toString() : QString();
}

} // namespace

class TestCanvasActionsWindow : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_canvas, m_head;

    canvas::CanvasStore store() const { return canvas::CanvasStore::forSession(); }
    QString head() const { return store().info(m_canvas)->head; }
    QObject *dialog(App &app) const { return app.window->findChild<QObject *>("canvasViewDialog"); }
    QObject *view(App &app) const { return dialog(app)->property("view").value<QObject *>(); }
    QQuickItem *taskBar(App &app) const { return find(app.window, "canvasTaskBar"); }
    void openDialog(App &app) {
        QVERIFY(app.commands.trigger("file.canvas-view"));
        QTRY_VERIFY(dialog(app) && dialog(app)->property("opened").toBool());
        QTRY_VERIFY(find(app.window, "canvasLayer-0"));
    }
    void click(App &app, const QString &name) {
        const QPoint at = settled(app.window, name);
        QVERIFY2(at.x() >= 0, qPrintable(name + " never settled"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, at);
    }
    // A rectangle drawn on the picture of the (first visible) Canvas view, `size` canvas pixels square from (160, 120).
    void drawRectangle(App &app, int size = 100) {
        QQuickItem *picture = find(app.window, "canvasViewViewer");
        QVERIFY(picture);
        QTRY_VERIFY(picture->property("imageScale").toReal() > 0);
        const auto at = [&](double px, double py) {
            const qreal k = picture->property("imageScale").toReal();
            return picture->mapToScene(QPointF(picture->property("imageX").toReal() + px * k, picture->property("imageY").toReal() + py * k)).toPoint();
        };
        const QPoint from = at(160, 120), to = at(160 + size, 120 + size);
        QTest::mouseMove(app.window, from);
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(app.window, from + (to - from) * i / 8);
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, to);
    }
    // The action menu's row `index` (0 Auto, then the actions in the list's order).
    void chooseAction(App &app, int index) {
        click(app, "canvasTaskAction");
        QObject *menu = nullptr;
        QTRY_VERIFY((menu = app.window->findChild<QObject *>("canvasTaskActionMenu")) && menu->property("opened").toBool());
        QMetaObject::invokeMethod(menu, "choose", Q_ARG(QVariant, index));
        QTRY_VERIFY(!app.window->findChild<QObject *>("canvasTaskActionMenu"));
    }
    int indexOf(Rig &r, const QString &id) const {
        const QVariantList rows = r.actions->actions();
        for (int i = 0; i < rows.size(); ++i)
            if (rows.at(i).toMap().value("id") == id)
                return i + 1; // after Auto
        return -1;
    }
    void typePrompt(App &app, const QString &text) {
        click(app, "canvasTaskPrompt");
        QTRY_VERIFY(find(app.window, "canvasTaskPrompt")->hasActiveFocus());
        typeInto(app.window, text);
        QTRY_COMPARE(textOf(app.window, "canvasTaskPrompt"), text);
    }

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        QVERIFY(m_dir.isValid());
        for (const char *variable : {"FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME", "FUGUE_BASE_XDG_STATE_HOME",
                                     "FUGUE_BASE_XDG_CACHE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR"})
            qunsetenv(variable);
        qputenv("FUGUE_HOME", m_dir.filePath("home").toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", m_dir.filePath("config").toLocal8Bit());
        qputenv("XDG_DATA_HOME", m_dir.filePath("data").toLocal8Bit());
        qputenv("XDG_STATE_HOME", m_dir.filePath("state").toLocal8Bit());
        qputenv("XDG_CACHE_HOME", m_dir.filePath("cache").toLocal8Bit());
        QDir().mkpath(m_dir.filePath("run"));
        qputenv("XDG_RUNTIME_DIR", m_dir.filePath("run").toLocal8Bit());
    }

    void init() {
        QDir(m_dir.filePath("home")).removeRecursively();
        PaintDocument d(512, 384);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(512, 384, {30, 60, 200, 255}));
        const auto made = store().create("Actions", d, "new");
        QVERIFY(made);
        m_canvas = made->id;
        m_head = made->head;
    }

    // The hobbyist's loop (design 9): drag a rectangle, type, Enter; progress and the shimmer while it runs; a layer.
    void aRectangleATypedPromptAndEnterLandALayer() {
        Rig r;
        QVERIFY(r.app.open());
        openDialog(r.app);
        QVERIFY(!taskBar(r.app)); // nothing selected: no bar over the picture
        drawRectangle(r.app);
        QTRY_VERIFY(taskBar(r.app));
        // Under the selection, 2u clear of it.
        QQuickItem *box = find(r.app.window, "canvasTaskBox");
        const QRectF selection = taskBar(r.app)->property("selectionBox").toRectF();
        QVERIFY(selection.width() > 0);
        QVERIFY(box->y() >= selection.bottom());
        QVERIFY(box->y() <= selection.bottom() + 3 * r.app.theme.look()->property("unit").toReal());
        // Auto picks Generative fill, whose models are not here: Generate is off; choose Test fill.
        QVERIFY(!find(r.app.window, "canvasTaskGenerate")->isEnabled());
        chooseAction(r.app, indexOf(r, "test-fill"));
        QCOMPARE(textOf(r.app.window, "canvasTaskAction"), QStringLiteral("Test fill"));
        typePrompt(r.app, "a red umbrella");
        QVERIFY(find(r.app.window, "canvasTaskGenerate")->isEnabled());
        QTest::keyClick(r.app.window, Qt::Key_Return);
        QTRY_COMPARE(r.jobs().size(), 1);
        QTRY_VERIFY(find(r.app.window, "canvasTaskCancel"));
        QVERIFY(!find(r.app.window, "canvasTaskGenerate"));
        r.onFake([&](FakeProvider &f) { f.progress(f.jobs().at(0), 0.45, "sampling"); });
        QTRY_COMPARE(textOf(r.app.window, "canvasTaskStatus"), QStringLiteral("Generating… 45%"));
        QVERIFY(find(r.app.window, "canvasTaskShimmer"));
        // The Sampler's step frame shows in the selection over the shimmer, and goes with the run.
        QVERIFY(!find(r.app.window, "canvasLiveFill"));
        const QString step = m_dir.filePath("step.png");
        QVERIFY(QImage(32, 32, QImage::Format_RGB32).save(step));
        emit r.service->event(r.actions->lastRun(), {{"event", "node.preview"}, {"node", "gen"},
                                                      {"thumbnails", QJsonObject{{"preview", QJsonObject{{"url", QUrl::fromLocalFile(step).toString()}}}}}});
        QTRY_VERIFY(find(r.app.window, "canvasLiveFill"));
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", png(64, 64)}}); });
        QTRY_VERIFY(!r.actions->busy());
        QTRY_VERIFY(!find(r.app.window, "canvasLiveFill")); // faded away
        QTRY_VERIFY(find(r.app.window, "canvasLayer-1")); // the layer, in the layers panel
        QVERIFY(head() != m_head);
        QTRY_VERIFY(find(r.app.window, "canvasTaskGenerate"));
        QVERIFY(!find(r.app.window, "canvasTaskShimmer"));
        QVERIFY(!r.runs->busy()); // the Graph's runs never saw it
        // Up in the empty field brings the prompt back.
        click(r.app, "canvasTaskPrompt");
        find(r.app.window, "canvasTaskPrompt")->setProperty("text", QString());
        QTest::keyClick(r.app.window, Qt::Key_Up);
        QTRY_COMPARE(textOf(r.app.window, "canvasTaskPrompt"), QStringLiteral("a red umbrella"));
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // Escape cancels this bar's run: nothing lands, nothing is said; an error is one plain line in the error color.
    void escapeCancelsAndAnErrorIsOnePlainLine() {
        Rig r;
        QVERIFY(r.app.open());
        openDialog(r.app);
        drawRectangle(r.app);
        QTRY_VERIFY(taskBar(r.app));
        chooseAction(r.app, indexOf(r, "test-fill"));
        typePrompt(r.app, "a cat");
        QTest::keyClick(r.app.window, Qt::Key_Return);
        QTRY_COMPARE(r.jobs().size(), 1);
        QSignalSpy finished(r.actions.get(), &CanvasActions::finished);
        QTest::keyClick(r.app.window, Qt::Key_Escape);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("cancelled"));
        QVERIFY(dialog(r.app)->property("opened").toBool()); // the Escape was the bar's, not the dialog's
        QCOMPARE(head(), m_head);
        QTRY_VERIFY(!find(r.app.window, "canvasTaskStatusRow"));
        // A failure: one line, the plain words.
        QTest::keyClick(r.app.window, Qt::Key_Return);
        QTRY_COMPARE(r.jobs().size(), 2);
        r.onFake([&](FakeProvider &f) { f.fail(f.jobs().at(1), makeError("mask_empty", "Nothing is painted in the mask.")); });
        QTRY_COMPARE(textOf(r.app.window, "canvasTaskStatus"), QStringLiteral("The selection is empty. Draw one, then Generate."));
        QCOMPARE(find(r.app.window, "canvasTaskStatus")->property("color").value<QColor>(),
                 r.app.theme.look()->property("error").value<QColor>());
        // Typing clears it.
        typeInto(r.app.window, "s");
        QTRY_VERIFY(!find(r.app.window, "canvasTaskStatusRow"));
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // Models missing (design 4): the line says how much, Download goes through the window's download line, Generate stays off.
    void missingModelsOfferDownloadAndKeepGenerateOff() {
        Rig r;
        QVERIFY(r.app.open());
        openDialog(r.app);
        drawRectangle(r.app);
        QTRY_VERIFY(taskBar(r.app));
        typePrompt(r.app, "a red umbrella");
        const QString status = textOf(r.app.window, "canvasTaskStatus");
        QVERIFY2(status.startsWith("Generative fill needs 3 models, "), qPrintable(status));
        QVERIFY(find(r.app.window, "canvasTaskDownload"));
        QQuickItem *generate = find(r.app.window, "canvasTaskGenerate");
        QVERIFY(!generate->isEnabled());
        QCOMPARE(generate->property("tip").toString(), QStringLiteral("Download the models first"));
        QTest::keyClick(r.app.window, Qt::Key_Return); // Enter does nothing either
        QTest::qWait(200);
        QCOMPARE(r.jobs().size(), 0);
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // Slice 6: an action on the current layer (a stand-in for Remove background, no pack) runs from the bar with one click, no prompt.
    void anActionOnTheCurrentLayerRunsInOneClick() {
        Rig r;
        r.addTemplate("lib-cut", kCutAction, "Cut");
        QVERIFY(r.app.open());
        openDialog(r.app);
        drawRectangle(r.app);
        QTRY_VERIFY(taskBar(r.app));
        chooseAction(r.app, indexOf(r, "lib-cut"));
        QTRY_VERIFY(find(r.app.window, "canvasTaskGenerate")->isEnabled());
        QVERIFY(!find(r.app.window, "canvasTaskInstallPack"));
        click(r.app, "canvasTaskGenerate");
        QTRY_VERIFY(head() != m_head);
        const auto tree = store().tree(store().revision(m_canvas, head())->tree);
        QCOMPARE(int(tree->layers.size()), 1);
        QVERIFY(tree->layers.front().hasMask);
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // The real template, whose pack is not installed here, shows what it needs with Install… and keeps Generate off. Install… asks
    // the window (Settings > Packs, which asks the person); nothing is installed by the test.
    void aMissingPackShowsWhatItNeedsAndInstallAsksTheWindow() {
        Rig r;
        r.addRepositoryTemplate("remove-background-action");
        QVERIFY(r.app.open());
        openDialog(r.app);
        drawRectangle(r.app);
        QTRY_VERIFY(taskBar(r.app));
        chooseAction(r.app, indexOf(r, "remove-background-action"));
        QTRY_COMPARE(textOf(r.app.window, "canvasTaskStatus"), QStringLiteral("Needs the Remove Background pack"));
        QQuickItem *generate = find(r.app.window, "canvasTaskGenerate");
        QVERIFY(!generate->isEnabled());
        QCOMPARE(generate->property("tip").toString(), QStringLiteral("Install the pack first"));
        QVERIFY(!find(r.app.window, "canvasTaskDownload")); // the models wait for the pack
        QSignalSpy asked(&r.app.workflows, &Workflows::packInstallRequested);
        click(r.app, "canvasTaskInstallPack");
        QCOMPARE(asked.size(), 1);
    }

    // The same bar in a Canvas area (#217): it works in either host.
    void theBarWorksInACanvasArea() {
        Rig r;
        QVERIFY(r.app.open());
        QVERIFY(r.app.commands.trigger("area.split-row"));
        QTRY_VERIFY(find(r.app.window, "canvasArea"));
        QTRY_VERIFY(find(r.app.window, "canvasLayer-0"));
        drawRectangle(r.app, 80);
        QTRY_VERIFY(taskBar(r.app));
        chooseAction(r.app, indexOf(r, "test-fill"));
        typePrompt(r.app, "a boat");
        QTest::keyClick(r.app.window, Qt::Key_Return);
        QTRY_COMPARE(r.jobs().size(), 1);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", png(64, 64)}}); });
        QTRY_VERIFY(!r.actions->busy());
        QTRY_VERIFY(find(r.app.window, "canvasLayer-1"));
        QCOMPARE(r.actions->error(), QString());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // Advanced > Edit workflow (design 7): the chosen action's graph opens as a tab and the Graph's area takes over.
    void editWorkflowShowsTheGraphInAnArea() {
        Rig r;
        QVERIFY(r.app.open());
        QVERIFY(r.app.commands.trigger("area.split-row"));
        QTRY_VERIFY(find(r.app.window, "canvasArea"));
        QTRY_VERIFY(find(r.app.window, "canvasLayer-0"));
        QVERIFY(r.app.workspace.active() != r.app.workspace.graphArea()); // the new area is the Canvas's
        drawRectangle(r.app, 80);
        QTRY_VERIFY(taskBar(r.app));
        chooseAction(r.app, indexOf(r, "test-fill"));
        click(r.app, "canvasTaskAdvancedButton");
        QTRY_VERIFY(r.app.window->findChild<QObject *>("canvasTaskAdvanced")->property("opened").toBool());
        click(r.app, "canvasTaskEdit"); // Edit workflow
        QTRY_COMPARE(r.app.workflows.tabs().at(r.app.workflows.current()).toMap().value("title").toString(), QStringLiteral("Test fill")); // replaces the blank tab
        QCOMPARE(r.app.workflows.tabs().at(r.app.workflows.current()).toMap().value("title").toString(), QStringLiteral("Test fill"));
        QCOMPARE(r.app.workspace.active(), r.app.workspace.graphArea());
        QCOMPARE(r.actions->error(), QString());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // The Advanced popover (slice 4): the action's other inputs through the Graph's widgets, a choice's words, Count, Reset.
    void theAdvancedPopoverEditsWhatGenerateUses() {
        Rig r;
        QVERIFY(r.app.open());
        openDialog(r.app);
        drawRectangle(r.app, 80);
        QTRY_VERIFY(taskBar(r.app));
        chooseAction(r.app, indexOf(r, "test-advanced"));
        click(r.app, "canvasTaskAdvancedButton");
        QObject *popover = nullptr;
        QTRY_VERIFY((popover = r.app.window->findChild<QObject *>("canvasTaskAdvanced")) && popover->property("opened").toBool());
        QTRY_VERIFY(find(r.app.window, "canvasTaskAdvancedWidget-strength"));
        QVERIFY(find(r.app.window, "canvasTaskAdvancedWidget-steps"));
        QVERIFY(!find(r.app.window, "canvasTaskAdvancedWidget-style")); // a choice of words has buttons instead
        QVERIFY(find(r.app.window, "canvasTaskAdvancedChoice-style-Light"));
        // The strength's widget shows the action's own default, 0.9.
        QQuickItem *strength = find(r.app.window, "canvasTaskAdvancedWidget-strength");
        QTRY_COMPARE(strength->property("number").toDouble(), 0.9);
        click(r.app, "canvasTaskAdvancedChoice-style-Restyle");
        QTRY_COMPARE(r.actions->choiceOf("test-advanced", "style"), QStringLiteral("Restyle"));
        QTRY_VERIFY(find(r.app.window, "canvasTaskAdvancedReset"));
        click(r.app, "canvasTaskCount-3");
        QTRY_COMPARE(r.actions->variations("test-advanced"), 3);
        // Generate runs the first of three with the word's number.
        typePrompt(r.app, "a boat");
        click(r.app, "canvasTaskGenerate");
        QTRY_COMPARE(r.jobs().size(), 1);
        QCOMPARE(r.actions->runTotal(), 3);
        QTRY_VERIFY(textOf(r.app.window, "canvasTaskStatus").startsWith("Generating 1 of 3"));
        QJsonObject params;
        r.onFake([&](FakeProvider &f) { params = f.submitted(f.jobs().at(0)).params; });
        QCOMPARE(params.value("guidance").toDouble(), 9.0);
        r.actions->cancel();
        QTRY_VERIFY(!r.actions->busy());
        QVERIFY(!popover->property("opened").toBool()); // a press on the bar's own fields closed it
        click(r.app, "canvasTaskAdvancedButton");
        QTRY_VERIFY(popover->property("opened").toBool());
        click(r.app, "canvasTaskAdvancedReset");
        QTRY_VERIFY(!r.actions->hasAdvancedChanges("test-advanced"));
        QCOMPARE(r.actions->variations("test-advanced"), 3); // Count stays
        QTest::keyClick(r.app.window, Qt::Key_Escape); // closes it
        QTRY_VERIFY(!popover->property("opened").toBool());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // In the dialog, which hides the Graph, Edit workflow closes it.
    void editWorkflowClosesTheDialog() {
        Rig r;
        QVERIFY(r.app.open());
        openDialog(r.app);
        drawRectangle(r.app, 80);
        QTRY_VERIFY(taskBar(r.app));
        chooseAction(r.app, indexOf(r, "test-fill"));
        click(r.app, "canvasTaskAdvancedButton");
        QTRY_VERIFY(r.app.window->findChild<QObject *>("canvasTaskAdvanced")->property("opened").toBool());
        click(r.app, "canvasTaskEdit");
        QTRY_COMPARE(r.app.workflows.tabs().at(r.app.workflows.current()).toMap().value("title").toString(), QStringLiteral("Test fill"));
        QTRY_VERIFY(!dialog(r.app)->property("opened").toBool());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // Pictures: FUGUE_SHOT_DIR=<dir> test_canvas_actions_window savesThePicturesOfTheBar (nothing without the variable),
    // dark and light: the bar under a rectangle, generating, the missing models, and a narrow area.
    void savesThePicturesOfTheBar_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("name");
        QTest::newRow("dark") << "tokyo-night" << "dark";
        QTest::newRow("light") << "catppuccin-latte" << "light";
    }
    void savesThePicturesOfTheBar() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("FUGUE_SHOT_DIR is not set");
        QFETCH(QString, theme);
        QFETCH(QString, name);
        const auto save = [&](App &app, const QString &what) {
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/canvas-taskbar-%2-%3.png").arg(QString::fromLocal8Bit(dir), name, what)));
        };
        {
            Rig r(R"({"fugue": 1, "graph": {"nodes": {}}})", Look{theme, 8});
            QVERIFY(r.app.open());
            openDialog(r.app);
            drawRectangle(r.app, 140);
            QTRY_VERIFY(taskBar(r.app));
            typePrompt(r.app, "a red umbrella");
            save(r.app, "missing");
            chooseAction(r.app, indexOf(r, "test-fill"));
            save(r.app, "ready");
            QTest::keyClick(r.app.window, Qt::Key_Return);
            QTRY_COMPARE(r.jobs().size(), 1);
            r.onFake([&](FakeProvider &f) { f.progress(f.jobs().at(0), 0.45, "sampling"); });
            QTRY_COMPARE(textOf(r.app.window, "canvasTaskStatus"), QStringLiteral("Generating… 45%"));
            save(r.app, "generating");
            r.onFake([&](FakeProvider &f) { f.fail(f.jobs().at(0), makeError("out_of_memory", "CUDA out of memory.")); });
            QTRY_VERIFY(!r.actions->busy());
            save(r.app, "error");
            chooseAction(r.app, indexOf(r, "test-advanced"));
            r.actions->setVariations("test-advanced", 2);
            click(r.app, "canvasTaskAdvancedButton");
            QTRY_VERIFY(find(r.app.window, "canvasTaskAdvancedWidget-strength"));
            {
                QObject *p = r.app.window->findChild<QObject *>("canvasTaskAdvanced");
                QTRY_VERIFY(p->property("y").toReal() + p->property("height").toReal() <= r.app.window->height()); // kept in the window
            }
            save(r.app, "advanced");
        }
        {
            Rig r(R"({"fugue": 1, "graph": {"nodes": {}}})", Look{theme, 8});
            QVERIFY(r.app.open());
            QVERIFY(r.app.commands.trigger("area.split-row"));
            QTRY_VERIFY(find(r.app.window, "canvasArea"));
            QTRY_VERIFY(find(r.app.window, "canvasLayer-0"));
            drawRectangle(r.app, 80);
            QTRY_VERIFY(taskBar(r.app));
            save(r.app, "area");
        }
    }
};

QTEST_MAIN(TestCanvasActionsWindow)
#include "test_canvas_actions_window.moc"
