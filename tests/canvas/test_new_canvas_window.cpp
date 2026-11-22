// The New canvas dialog in the real window (Main.qml, offscreen) (#232, epic #206, spec 14.5, 6.2, 12.4): File > New
// canvas… (canvas.new) opens it on the last settings, a preset or a typed size and a background make a blank canvas,
// the clipboard's picture is offered with its size and makes the canvas, a size over the cap is refused in words, Paste
// as new canvas needs a picture and asks nothing, and the canvas input's New canvas… opens the same dialog and puts the
// new id in the input. The store and the settings are the test's own (FUGUE_HOME and the XDG folders point at a
// temporary folder); QTest events only, QClipboard in the test process; no real window or pointer.

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QUndoStack>

#include "canvas/widget_edit.h"
#include "canvas_app.h"
#include "engine/canvas_store.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::load;

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

const char *regionFixture = R"({
  "fugue": 1,
  "graph": {"nodes": {"region": {"type": "fugue/canvas-region", "version": "1.0.0"}}},
  "layout": {"nodes": {"region": {"pos": [20, 20]}}, "view": {"center": [200, 200], "zoom": 1}}
})";

} // namespace

class TestNewCanvasWindow : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    int m_run = 0;

    static canvas::CanvasStore store() { return canvas::CanvasStore::forSession(); }
    static QObject *dialog(App &app) { return app.window->findChild<QObject *>("newCanvasDialog"); }
    static bool dialogOpen(App &app) { return dialog(app) && dialog(app)->property("opened").toBool(); }
    static QObject *model(App &app) { return dialog(app)->property("model").value<QObject *>(); }
    static void click(App &app, const QString &name) {
        QTest::qWait(20); // a Flow lays out its rows on the next frame, as a person would see them
        QQuickItem *item = find(app.window, name);
        QVERIFY2(item, qPrintable(name));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(item));
    }
    // What a person does: click the field, select its text, type over it.
    static void type(App &app, const QString &field, const QString &text) {
        click(app, field);
        QTest::keyClick(app.window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(app.window, Qt::Key_Delete);
        for (const QChar c : text)
            QTest::keyClick(app.window, c.toLatin1());
    }
    static QString text(App &app, const QString &name) {
        QQuickItem *item = find(app.window, name);
        return item ? item->property("text").toString() : QStringLiteral("<none>");
    }
    static void openFromMenu(App &app) {
        QVERIFY(app.commands.trigger("canvas.new"));
        QTRY_VERIFY(dialogOpen(app));
    }
    static void putOnClipboard(const QImage &image) {
        QGuiApplication::clipboard()->setImage(image);
        QTRY_VERIFY(QGuiApplication::clipboard()->mimeData()->hasImage());
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

    // Each test starts from an empty store, settings and clipboard.
    void init() {
        for (const char *folder : {"home", "data", "state"})
            QDir(m_dir.filePath(folder)).removeRecursively();
        QGuiApplication::clipboard()->clear();
        ++m_run;
    }

    void theCommandOpensTheDialogOnABlankSquareAndCancelMakesNothing() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        QVERIFY(app.commands.keys("canvas.new").contains("Ctrl+Alt+N"));
        QVERIFY(!dialogOpen(app) || !find(app.window, "newCanvasName"));
        openFromMenu(app);
        QCOMPARE(text(app, "newCanvasName"), QString("Untitled 1"));
        QCOMPARE(text(app, "newCanvasWidth"), QString("1024"));
        QCOMPARE(text(app, "newCanvasHeight"), QString("1024"));
        QVERIFY(find(app.window, "newCanvasSummary")->property("text").toString().contains("1.05 megapixels"));
        QVERIFY(!find(app.window, "newCanvasProblem"));
        // Clipboard is not offered without a picture; it explains itself.
        QVERIFY(!find(app.window, "newCanvasSourceClipboard")->isEnabled());
        QCOMPARE(find(app.window, "newCanvasSourceClipboard")->property("tip").toString(), QString("Copy a picture first"));
        click(app, "newCanvasCancel");
        QTRY_VERIFY(!dialogOpen(app));
        QVERIFY(store().list().empty());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aPresetAndABackgroundMakeTheCanvasAndTheCanvasViewShowsIt() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openFromMenu(app);
        click(app, "newCanvasPreset-hd");
        QCOMPARE(text(app, "newCanvasWidth"), QString("1920"));
        QCOMPARE(text(app, "newCanvasHeight"), QString("1080"));
        QVERIFY(find(app.window, "newCanvasPreset-hd")->property("current").toBool());
        click(app, "newCanvasBackground-white");
        type(app, "newCanvasName", "Poster");
        click(app, "newCanvasOk");
        QTRY_VERIFY(!dialogOpen(app));
        const std::vector<canvas::CanvasInfo> made = store().list();
        QCOMPARE(int(made.size()), 1);
        QCOMPARE(made[0].name, QString("Poster"));
        const mask::PaintDocument doc = store().load(made[0].id, made[0].head).value();
        QCOMPARE(doc.width(), 1920);
        QCOMPARE(doc.layer(0).name, QString("Background"));
        // With no Canvas area, the canvas opens in the Canvas view's large view, as Photoshop opens the document it made.
        QObject *view = app.window->findChild<QObject *>("canvasViewDialog");
        QVERIFY(view);
        QTRY_VERIFY(view->property("opened").toBool());
        QCOMPARE(view->property("view").value<QObject *>()->property("canvasId").toString(), made[0].id);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void typingASizeKeepsTheKeyboardWorkingAndTheLastSettingsComeBack() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openFromMenu(app);
        type(app, "newCanvasWidth", "640");
        type(app, "newCanvasHeight", "480");
        QVERIFY(find(app.window, "newCanvasPreset-custom")->property("current").toBool());
        click(app, "newCanvasBackground-colour");
        type(app, "newCanvasColour", "#102030");
        QTest::keyClick(app.window, Qt::Key_Return); // Enter in a field makes it
        QTRY_VERIFY(!dialogOpen(app));
        const canvas::CanvasInfo info = store().list().at(0);
        QCOMPARE(store().load(info.id, info.head)->width(), 640);
        // Closed from the Canvas view's dialog, then again: the settings are the last ones, the name the next free.
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTest::qWait(50);
        openFromMenu(app);
        QCOMPARE(text(app, "newCanvasWidth"), QString("640"));
        QCOMPARE(text(app, "newCanvasHeight"), QString("480"));
        QVERIFY(find(app.window, "newCanvasBackground-colour")->property("current").toBool());
        QCOMPARE(text(app, "newCanvasColour"), QString("#102030"));
        QCOMPARE(text(app, "newCanvasName"), QString("Untitled 2"));
    }

    void aSizeOverTheCapIsRefusedInWordsAndCreateWaits() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openFromMenu(app);
        type(app, "newCanvasWidth", "20000");
        QTRY_VERIFY(find(app.window, "newCanvasProblem"));
        QVERIFY(find(app.window, "newCanvasProblem")->property("text").toString().contains("at most 16384 px a side"));
        QVERIFY(!find(app.window, "newCanvasOk")->isEnabled());
        click(app, "newCanvasOk");
        QVERIFY(dialogOpen(app));
        QVERIFY(store().list().empty());
        type(app, "newCanvasWidth", "");
        QVERIFY(find(app.window, "newCanvasProblem")->property("text").toString().contains("whole numbers"));
        type(app, "newCanvasWidth", "2000");
        QTRY_VERIFY(!find(app.window, "newCanvasProblem"));
        QVERIFY(find(app.window, "newCanvasOk")->isEnabled());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theClipboardPictureIsOfferedWithItsSizeAndMakesTheCanvas() {
        QImage image(300, 120, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        putOnClipboard(image);
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openFromMenu(app);
        QVERIFY(find(app.window, "newCanvasSourceClipboard")->property("current").toBool());
        QCOMPARE(text(app, "newCanvasWidth"), QString("300"));
        QCOMPARE(text(app, "newCanvasHeight"), QString("120"));
        QVERIFY(!find(app.window, "newCanvasWidth")->isEnabled());
        QVERIFY(!find(app.window, "newCanvasPreset-hd")); // presets are for a blank canvas
        QCOMPARE(find(app.window, "newCanvasSourceLine")->property("text").toString(), QString("the picture on the clipboard"));
        click(app, "newCanvasOk");
        QTRY_VERIFY(!dialogOpen(app));
        const canvas::CanvasInfo info = store().list().at(0);
        const mask::PaintDocument doc = store().load(info.id, info.head).value();
        QCOMPARE(doc.width(), 300);
        QCOMPARE(doc.height(), 120);
        QCOMPARE(doc.pixels(doc.layer(0).id)->rgbaAt(10, 10), (mask::Rgba{255, 0, 0, 255}));
        // Blank instead: the typed size is back.
        openFromMenu(app);
        click(app, "newCanvasSourceBlank");
        QCOMPARE(text(app, "newCanvasWidth"), QString("1024"));
        QVERIFY(find(app.window, "newCanvasWidth")->isEnabled());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void pasteAsNewCanvasNeedsAPictureAndAsksNothing() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        QVERIFY(!app.commands.isEnabled("canvas.paste-new"));
        QVERIFY(!app.commands.trigger("canvas.paste-new"));
        QImage image(64, 32, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        putOnClipboard(image);
        QTRY_VERIFY(app.commands.isEnabled("canvas.paste-new"));
        QVERIFY(app.commands.trigger("canvas.paste-new"));
        QTRY_COMPARE(int(store().list().size()), 1);
        QCOMPARE(store().list().at(0).name, QString("Pasted 1"));
        QVERIFY(!dialogOpen(app)); // no dialog
        QTRY_VERIFY(app.window->findChild<QObject *>("canvasViewDialog")->property("opened").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theFileMenuHasBothRows() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        const QVariantList bar = app.commands.menu("bar");
        QStringList ids;
        for (const QVariant &top : bar)
            if (top.toMap().value("title").toString().contains("File"))
                for (const QVariant &row : top.toMap().value("items").toList())
                    ids << row.toMap().value("command").toString();
        QVERIFY2(ids.contains("canvas.new") && ids.contains("canvas.paste-new"), qPrintable(ids.join(',')));
        QVERIFY(ids.indexOf("canvas.new") < ids.indexOf("file.canvas-view"));
    }

    // A canvas input's menu: New canvas… opens the same dialog; the new id is the input's value, one undo step.
    void theCanvasInputsNewCanvasIsThisDialog() {
        App app(load(regionFixture));
        app.model.widgets()->canvasChoices()->setStoreRoot(QString()); // the session's store, as the command's dialog
        QVERIFY(app.open());
        QQuickItem *widget = nullptr;
        QList<QQuickItem *> todo{app.window->contentItem()};
        while (!todo.isEmpty() && !widget) {
            QQuickItem *at = todo.takeFirst();
            if (at->objectName() == "inputWidget" && at->property("name").toString() == "canvas")
                widget = at;
            todo << at->childItems();
        }
        QVERIFY(widget);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(widget));
        QObject *pickers = widget->property("pickers").value<QObject *>();
        QVERIFY(pickers);
        QObject *menu = pickers->findChild<QObject *>("widgetMenu");
        QTRY_VERIFY(menu && menu->property("opened").toBool());
        const QVariantList shown = menu->property("shown").toList();
        for (int i = 0; i < shown.size(); ++i)
            if (shown[i].toMap().value("text").toString() == "New canvas…")
                QVERIFY(QMetaObject::invokeMethod(menu, "choose", Q_ARG(QVariant, i)));
        QTRY_VERIFY(find(app.window, "newCanvasName"));
        QObject *picked = pickers->findChild<QObject *>("newCanvasDialog");
        QVERIFY(picked);
        QTRY_VERIFY(picked->property("opened").toBool());
        QCOMPARE(text(app, "newCanvasName"), QString("Untitled 1"));
        click(app, "newCanvasPreset-landscape");
        type(app, "newCanvasName", "Board");
        click(app, "newCanvasOk");
        QTRY_VERIFY(!picked->property("opened").toBool());
        const std::vector<canvas::CanvasInfo> made = store().list();
        QCOMPARE(int(made.size()), 1);
        QCOMPARE(made[0].name, QString("Board"));
        QCOMPARE(app.model.document().graph.nodes.value("region").inputs.value("canvas").toJson(), QJsonValue(made[0].id));
        QCOMPARE(app.model.undoStack()->count(), 1);
        QCOMPARE(store().load(made[0].id, made[0].head)->width(), 1024);
        // The input's dialog opened the one canvas view? No: the picker asked, so the picker's node has it, nothing else opens.
        QVERIFY(!app.window->findChild<QObject *>("canvasViewDialog"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(TestNewCanvasWindow)
#include "test_new_canvas_window.moc"
