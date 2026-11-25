// The dialogs in the Drift kit (#478, rest of #465), offscreen: one keyboard test per dialog restyled (About, the shortcuts help, the Large
// view, the export dialog, a Question form), driven by keys alone: open, move with Tab, act, Escape closes. With FUGUE_DIALOG_SHOTS=<dir>
// (and FUGUE_SHOT_THEME, FUGUE_SHOT_LOOK="preset=flat", FUGUE_SHOT_TAG) a last test saves each dialog and the layers panel as pictures.

#include <QDir>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas_app.h"
#include "../window_grab.h"
#include "engine/canvas_store.h"
#include "../test_shard.h"

using namespace canvas_test;
using namespace fugue;
using namespace fugue::mask;

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

// Clicks once the layout has settled.
void click(App &app, const QString &name) {
    QPoint at;
    int still = 0;
    for (int i = 0; still < 3 && i < 500; ++i) {
        QTest::qWait(20);
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

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

const char *emptyDoc = R"({"fugue": 1, "graph": {"nodes": {}}})";

} // namespace

class TestDialogsKitWindow : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QObject *named(App &app, const char *name) const { return app.window->findChild<QObject *>(name); }
    bool isOpen(App &app, const char *name) const {
        const QObject *box = named(app, name);
        return box && box->property("opened").toBool();
    }
    // The focus is inside the popup's content.
    bool focusIn(App &app, const char *popup) const {
        const QObject *box = named(app, popup);
        if (!box)
            return false;
        const auto *content = qobject_cast<QQuickItem *>(box->property("contentItem").value<QObject *>());
        for (QQuickItem *at = app.window->activeFocusItem(); at; at = at->parentItem())
            if (at == content)
                return true;
        return false;
    }
    // The focus is anywhere in the popup (its frame, header or content): its keys are the popup's.
    bool focusOnPopup(App &app, const char *popup) const {
        const QObject *box = named(app, popup);
        if (!box)
            return false;
        const auto *content = qobject_cast<QQuickItem *>(box->property("contentItem").value<QObject *>());
        const QQuickItem *frame = content ? content->parentItem() : nullptr;
        for (QQuickItem *at = app.window->activeFocusItem(); at && frame; at = at->parentItem())
            if (at == frame)
                return true;
        return false;
    }
    void openCanvasView(App &app) {
        QVERIFY(app.commands.trigger("file.canvas-view"));
        QTRY_VERIFY(isOpen(app, "canvasViewDialog"));
        QTRY_VERIFY(find(app.window, "canvasLayer-2"));
    }
    // Canvas > Export > the row, as a person does with the pointer; the keys take over from there.
    void openExport(App &app, const QString &command) {
        click(app, "canvasMenuTitle-Canvas");
        click(app, "subMenu-Export");
        click(app, "menuItem-" + command);
        QTRY_VERIFY(isOpen(app, "canvasExportDialog"));
    }
    void openAbout(App &app) {
        QVERIFY(app.commands.trigger("help.about"));
        QTRY_VERIFY(isOpen(app, "about"));
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
        QDir(m_dir.filePath("data")).removeRecursively();
        QDir(m_dir.filePath("config")).removeRecursively();
        PaintDocument d(256, 256);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(256, 256, {30, 60, 200, 255}));
        LayerInfo fill;
        fill.name = "Fill 1";
        fill.producer = "canvas-write:write";
        fill.created = "2026-11-22T10:00:00.000Z";
        const MaskPaint mask(256, 256, std::vector<uint8_t>(256 * 256, 255), 1);
        d.loadLayer(fill, solid(256, 256, {200, 10, 10, 255}), &mask);
        d.loadLayer(LayerKind::Paint, "Fill 2", true, 1, solid(256, 256, {10, 200, 10, 128}));
        QVERIFY(canvas::CanvasStore::forSession().create("Layers", d, "new"));
    }

    // About: opens on its Close button, Enter closes it; Escape closes too.
    void aboutIsKeyboardOnly() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        openAbout(app);
        QVERIFY(named(app, "aboutHero"));
        QTRY_VERIFY(find(app.window, "aboutClose")->hasActiveFocus());
        app.key(Qt::Key_Return);
        QTRY_VERIFY(!isOpen(app, "about"));
        openAbout(app);
        QTRY_VERIFY(find(app.window, "aboutClose")->hasActiveFocus());
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app, "about"));
        // Wait for About's exit to end: when it does, the popup gives the focus back, and a dialog opened during the fade would lose
        // it (#503). Then the keys are the window's again: the next dialog hears its Escape.
        QTRY_VERIFY(!named(app, "about")->property("visible").toBool());
        QVERIFY(app.commands.trigger("app.shortcuts"));
        QTRY_VERIFY(isOpen(app, "help"));
        QTRY_VERIFY(focusOnPopup(app, "help"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app, "help"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The shortcuts help: Tab goes to the hero's close, then the filter; typing narrows the list; Escape closes.
    void theShortcutsHelpIsKeyboardOnly() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("app.shortcuts"));
        QTRY_VERIFY(isOpen(app, "help"));
        QVERIFY(named(app, "helpHero"));
        app.key(Qt::Key_Escape); // straight away: the keys are the popup's from the start
        QTRY_VERIFY(!isOpen(app, "help"));
        QVERIFY(app.commands.trigger("app.shortcuts"));
        QTRY_VERIFY(isOpen(app, "help"));
        const int before = find(app.window, "commandList")->property("contentHeight").toInt();
        app.key(Qt::Key_Tab);
        app.key(Qt::Key_Tab);
        QTRY_VERIFY(find(app.window, "helpFilter")->hasActiveFocus());
        for (const char letter : QByteArray("zoom"))
            QTest::keyClick(app.window, letter);
        QTRY_VERIFY(find(app.window, "commandList")->property("contentHeight").toInt() < before);
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app, "help"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The Large view (the Canvas view): Tab moves through the bar's buttons, Escape closes it.
    void theLargeViewIsKeyboardOnly() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        openCanvasView(app);
        QVERIFY(named(app, "canvasViewDialog")->findChild<QObject *>("largeViewFade"));
        QTRY_VERIFY(focusIn(app, "canvasViewDialog"));
        app.key(Qt::Key_Tab);
        QTRY_VERIFY(app.window->activeFocusItem() != nullptr);
        QQuickItem *first = app.window->activeFocusItem();
        app.key(Qt::Key_Tab);
        QTRY_VERIFY(app.window->activeFocusItem() != first);
        QVERIFY(focusIn(app, "canvasViewDialog"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app, "canvasViewDialog"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The export dialog: opens on a control of its own, Tab walks on, Escape cancels.
    void theExportDialogIsKeyboardOnly() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        openCanvasView(app);
        openExport(app, "canvas-view.export-options");
        QTRY_VERIFY(focusIn(app, "canvasExportDialog"));
        QQuickItem *first = app.window->activeFocusItem();
        app.key(Qt::Key_Tab);
        QTRY_VERIFY(app.window->activeFocusItem() != first);
        QVERIFY(focusIn(app, "canvasExportDialog"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app, "canvasExportDialog"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // A form Question (New canvas): opens on its name field, Tab leaves it, Escape closes.
    void aQuestionFormIsKeyboardOnly() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("canvas.new"));
        QTRY_VERIFY(isOpen(app, "newCanvasDialog"));
        QVERIFY(named(app, "newCanvasDialog")->property("heading").toString() == "New canvas");
        QTRY_VERIFY(find(app.window, "newCanvasName")->hasActiveFocus());
        app.key(Qt::Key_Tab);
        QTRY_VERIFY(!find(app.window, "newCanvasName")->hasActiveFocus());
        QVERIFY(focusIn(app, "newCanvasDialog"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app, "newCanvasDialog"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // FUGUE_DIALOG_SHOTS=<dir> [FUGUE_SHOT_THEME=<omarchy theme>] [FUGUE_SHOT_LOOK="preset=flat"] [FUGUE_SHOT_TAG=<tag>]: each dialog and the layers panel.
    void screenshots() {
        const QString out = qEnvironmentVariable("FUGUE_DIALOG_SHOTS");
        if (out.isEmpty())
            QSKIP("Set FUGUE_DIALOG_SHOTS to a folder to write the pictures there.");
        QDir().mkpath(out);
        const QString tag = qEnvironmentVariable("FUGUE_SHOT_TAG", QStringLiteral("shot"));
        App app(load(emptyDoc), {}, Look{qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("tokyo-night")), 12});
        QVERIFY(app.open());
        for (const QString &part : qEnvironmentVariable("FUGUE_SHOT_LOOK").split(';', Qt::SkipEmptyParts)) {
            const QStringList kv = part.split('=');
            QVERIFY(kv.size() == 2);
            QCOMPARE(kv[0] == "preset" ? app.settings.applyLookPreset(kv[1]) : app.settings.set(kv[0], kv[1].toDouble()), QString());
        }
        app.window->resize(1888, 971);
        QTest::qWait(300);
        const auto save = [&](const QString &what) {
            QTest::qWait(350);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/%2-%3.png").arg(out, tag, what)));
        };
        QTest::mouseMove(app.window, QPoint(40, 600));
        openAbout(app);
        save("about");
        QVERIFY(QMetaObject::invokeMethod(named(app, "about"), "close"));
        QTRY_VERIFY(!isOpen(app, "about"));
        QVERIFY(app.commands.trigger("app.shortcuts"));
        QTRY_VERIFY(isOpen(app, "help"));
        save("help");
        QVERIFY(QMetaObject::invokeMethod(named(app, "help"), "close"));
        QTRY_VERIFY(!isOpen(app, "help"));
        QVERIFY(app.commands.trigger("canvas.new"));
        QTRY_VERIFY(isOpen(app, "newCanvasDialog"));
        save("new-canvas");
        QVERIFY(QMetaObject::invokeMethod(named(app, "newCanvasDialog"), "close"));
        QTRY_VERIFY(!isOpen(app, "newCanvasDialog"));
        openCanvasView(app);
        save("large-view-layers");
        openExport(app, "canvas-view.export-options");
        save("export");
        QVERIFY(QMetaObject::invokeMethod(named(app, "canvasExportDialog"), "close"));
        QTRY_VERIFY(!isOpen(app, "canvasExportDialog"));
        QVERIFY(QMetaObject::invokeMethod(named(app, "canvasViewDialog"), "close"));
        QTRY_VERIFY(!isOpen(app, "canvasViewDialog"));
        // The notice of a workflow with a node this install lacks.
        app.workflows.openText(QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {"a": {"type": "nowhere/missing", "version": "1.0.0"}}}})"));
        QTRY_VERIFY(find(app.window, "workflowNotice"));
        save("notice");
    }
};

FUGUE_TEST_MAIN(TestDialogsKitWindow, QGuiApplication)
#include "test_dialogs_kit_window.moc"
