// The Move tool and Free Transform of the Canvas view in the real window (#236), offscreen, driven by QTest events only (no real
// window, no real pointer): V and a drag moves the current layer as ONE revision when the button is let go; the arrow keys nudge
// it; Ctrl+T opens the box with its handles and numbers, nothing is stored until Enter (one revision), Escape leaves the layer as
// it was; a group is refused in words.

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas_app.h"
#include "../window_grab.h"
#include "canvas/mask_layer.h"
#include "engine/canvas_store.h"

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

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

} // namespace

class TestCanvasTransformWindow : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_canvas, m_head;

    QObject *dialog(App &app) const { return app.window->findChild<QObject *>("canvasViewDialog"); }
    QObject *view(App &app) const { return dialog(app)->property("view").value<QObject *>(); }
    QObject *model(App &app) const { return view(app)->property("backend").value<QObject *>(); }
    MaskLayer *layerMask(App &app) const { return view(app)->property("layerMaskItem").value<MaskLayer *>(); }
    MaskLayer *selection(App &app) const { return view(app)->property("selectionMask").value<MaskLayer *>(); }
    int revisions() const { return int(canvas::CanvasStore::forSession().log(m_canvas).value().size()); }
    QString head() const { return canvas::CanvasStore::forSession().info(m_canvas)->head; }
    void openDialog(App &app) {
        QVERIFY(app.commands.trigger("file.canvas-view"));
        QTRY_VERIFY(dialog(app) && dialog(app)->property("opened").toBool());
        QTRY_VERIFY(find(app.window, "canvasLayer-1"));
        QTRY_VERIFY(find(app.window, "canvasViewToolStrip"));
    }
    void click(App &app, const QString &name, Qt::KeyboardModifiers mods = {}) {
        const QPoint at = settled(app.window, name);
        QVERIFY2(at.x() >= 0, qPrintable(name + " never settled"));
        QTest::mouseClick(app.window, Qt::LeftButton, mods, at);
    }
    // The pointer (QTest's) over the picture: the view's keys are live.
    QPoint hoverPicture(App &app) {
        const QPoint at = settled(app.window, "canvasViewViewer");
        QTest::mouseMove(app.window, at + QPoint(3, 3)); // the pointer may be where an earlier test left it: a move is needed to hover
        QTest::mouseMove(app.window, at);
        [&] { QTRY_VERIFY(view(app)->property("keysHot").toBool()); }();
        return at;
    }
    void key(App &app, int k, Qt::KeyboardModifiers mods = {}) { QTest::keyClick(app.window, Qt::Key(k), mods); }
    // A drag from `from` by (dx, dy) screen pixels.
    void drag(App &app, QPoint from, int dx, int dy = 0) {
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(app.window, from + QPoint(dx * i / 8, dy * i / 8));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, from + QPoint(dx, dy));
    }
    canvas::Tree headTree() const { return canvas::CanvasStore::forSession().loadTree(m_canvas, head()).value(); }

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
        MaskPaint card(256, 256, 4); // a 64 x 64 card in the middle of the top layer
        std::vector<uint8_t> px(256 * 256 * 4, 0);
        for (int y = 96; y < 160; ++y)
            for (int x = 96; x < 160; ++x)
                for (int k = 0; k < 4; ++k)
                    px[(size_t(y) * 256 + size_t(x)) * 4 + size_t(k)] = k == 0 ? 205 : k == 1 ? 195 : k == 2 ? 160 : 255;
        d.loadLayer(LayerKind::Paint, "Card", true, 1, MaskPaint(256, 256, px, 4));
        const auto made = canvas::CanvasStore::forSession().create("Move", d, "new");
        QVERIFY(made);
        m_canvas = made->id;
        m_head = made->head;
    }

    // V takes the Move tool; a drag on the picture moves the current layer and is ONE revision when the button is let go.
    void theMoveToolDragsTheCurrentLayerAsOneRevision() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QVERIFY(find(app.window, "canvasViewTool-move")->property("tip").toString().endsWith("(V)"));
        const QPoint at = hoverPicture(app);
        key(app, Qt::Key_V);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("move"));
        QCOMPARE(find(app.window, "canvasViewToolName")->property("text").toString(), QString("Move"));
        QCOMPARE(revisions(), 1);
        // Mid drag nothing is stored; the layer is drawn at its new place and the canvas under it has it left out.
        QTest::mousePress(app.window, Qt::LeftButton, {}, at);
        QTest::mouseMove(app.window, at + QPoint(20, 10));
        QTest::mouseMove(app.window, at + QPoint(40, 20));
        QTRY_VERIFY(model(app)->property("transforming").toBool());
        QCOMPARE(revisions(), 1);
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, at + QPoint(40, 20));
        QTRY_COMPARE(revisions(), 2);
        QTRY_VERIFY(!model(app)->property("transforming").toBool());
        const canvas::Tree after = headTree();
        QVERIFY(after.layers[1].source.has_value());
        QVERIFY2(after.layers[1].source->transform.cx > 128.0, "the card moved right");
        QVERIFY(after.layers[1].source->transform.cy > 128.0);
        QCOMPARE(after.layers[1].source->width, 64); // the card's bounds, not the canvas
        QCOMPARE(model(app)->property("steps").toList().first().toMap().value("name").toString(), QString("Layer moved: Card"));
        // The arrow keys nudge it: one pixel, and with Shift ten, each a revision.
        const double before = after.layers[1].source->transform.cx;
        view(app)->findChild<QObject *>("canvasViewViewer")->metaObject();
        QMetaObject::invokeMethod(view(app)->findChild<QObject *>("canvasViewViewer"), "forceActiveFocus");
        key(app, Qt::Key_Right);
        QTRY_COMPARE(revisions(), 3);
        QCOMPARE(headTree().layers[1].source->transform.cx, before + 1);
        key(app, Qt::Key_Down, Qt::ShiftModifier);
        QTRY_COMPARE(revisions(), 4);
        QCOMPARE(headTree().layers[1].source->transform.cy - after.layers[1].source->transform.cy, 10.0);
        // A press that does not move makes nothing.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, at);
        QTest::qWait(100);
        QCOMPARE(revisions(), 4);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Ctrl+T: the box with eight handles and the numbers; a handle drag changes the draft only; Enter keeps it as one revision.
    void freeTransformScalesWithAHandleAndEnterKeepsItAsOneRevision() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        hoverPicture(app);
        key(app, Qt::Key_T, Qt::ControlModifier);
        QTRY_VERIFY(view(app)->property("freeTransform").toBool());
        QCOMPARE(find(app.window, "canvasViewToolName")->property("text").toString(), QString("Free transform"));
        for (const char *name : {"top-left", "top", "top-right", "right", "bottom-right", "bottom", "bottom-left", "left"})
            QVERIFY2(settled(app.window, QString("transformHandle-") + name).x() >= 0, name);
        QVERIFY(find(app.window, "canvasViewTransformW"));
        QVERIFY(find(app.window, "canvasViewTransformApply"));
        const double width = view(app)->property("xfPlacement").toMap().value("width").toDouble();
        QCOMPARE(width, 64.0);
        const QPoint handle = settled(app.window, "transformHandle-bottom-right");
        drag(app, handle, -20, -20); // 20 screen pixels in toward the layer
        QVERIFY(view(app)->property("xfPlacement").toMap().value("width").toDouble() < width);
        QCOMPARE(revisions(), 1); // nothing stored yet
        // The numbers: W with the link on scales H along.
        view(app)->setProperty("xfLink", true);
        QObject *field = find(app.window, "canvasViewTransformW");
        QVERIFY(QMetaObject::invokeMethod(field, "committed", Q_ARG(double, 32.0)));
        QCOMPARE(view(app)->property("xfPlacement").toMap().value("width").toDouble(), 32.0);
        QCOMPARE(view(app)->property("xfPlacement").toMap().value("height").toDouble(), 32.0);
        key(app, Qt::Key_Return);
        QTRY_COMPARE(revisions(), 2);
        QTRY_VERIFY(!view(app)->property("freeTransform").toBool());
        const canvas::LayerTransform t = headTree().layers[1].source->transform;
        QCOMPARE(t.width, 32.0);
        QCOMPARE(t.height, 32.0);
        QCOMPARE(model(app)->property("steps").toList().first().toMap().value("name").toString(), QString("Layer scaled: Card"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Ctrl+V over the view is a layer only when the clipboard has a picture; else (nodes, text, or an earlier step shown) it is the Graph's paste.
    void ctrlVFallsThroughToTheGraphsPasteWhenThereIsNoPictureToPlace() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        int graphPastes = 0;
        QVERIFY(app.commands.setHandler("edit.paste", [&] { ++graphPastes; }));
        app.commands.setEnabledWhen("edit.paste", [] { return true; });
        QGuiApplication::clipboard()->setText("some nodes");
        QTRY_VERIFY(!QGuiApplication::clipboard()->mimeData()->hasImage());
        hoverPicture(app);
        key(app, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(graphPastes, 1);
        QCOMPARE(revisions(), 1);
        // With a picture on the clipboard it is a layer, and the Graph's paste does not run.
        QImage image(20, 10, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        QGuiApplication::clipboard()->setImage(image);
        QTRY_VERIFY(QGuiApplication::clipboard()->mimeData()->hasImage());
        key(app, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(revisions(), 2);
        QCOMPARE(graphPastes, 1);
        // A step shown that is not the latest: the command is off, and the key is still the Graph's.
        QVERIFY(QMetaObject::invokeMethod(model(app), "show", Q_ARG(QString, m_head)));
        QTRY_VERIFY(!model(app)->property("layersEditable").toBool());
        key(app, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(graphPastes, 2);
        QCOMPARE(revisions(), 2);
    }

    void escapeLeavesTheLayerAsItWasAndAGroupIsRefusedInWords() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        hoverPicture(app);
        key(app, Qt::Key_T, Qt::ControlModifier);
        QTRY_VERIFY(view(app)->property("freeTransform").toBool());
        const QPoint handle = settled(app.window, "transformHandle-right");
        drag(app, handle, 30);
        QVERIFY(view(app)->property("xfPlacement").toMap().value("width").toDouble() > 64);
        key(app, Qt::Key_Escape);
        QTRY_VERIFY(!view(app)->property("freeTransform").toBool());
        QVERIFY(!model(app)->property("transforming").toBool());
        QCOMPARE(revisions(), 1);
        // Apply with nothing changed makes no revision.
        key(app, Qt::Key_T, Qt::ControlModifier);
        QTRY_VERIFY(view(app)->property("freeTransform").toBool());
        click(app, "canvasViewTransformApply");
        QTRY_VERIFY(!view(app)->property("freeTransform").toBool());
        QCOMPARE(revisions(), 1);
        // A group cannot be transformed: Ctrl+T says so and opens nothing.
        QVERIFY(QMetaObject::invokeMethod(model(app), "groupLayers", Q_ARG(QVariantList, QVariantList{1})));
        QTRY_COMPARE(revisions(), 2);
        key(app, Qt::Key_T, Qt::ControlModifier);
        QTRY_VERIFY(model(app)->property("notice").toString().contains("paint layer"));
        QVERIFY(!view(app)->property("freeTransform").toBool());
        QCOMPARE(revisions(), 2);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(TestCanvasTransformWindow)
#include "test_canvas_transform_window.moc"
