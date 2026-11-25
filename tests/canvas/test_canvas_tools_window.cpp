// The Canvas view's tool strip and mask brush in the real window (#230), offscreen, driven by QTest events only (no real
// window, no real pointer): the strip's tools with their keys while the pointer is over the view; Add mask in the layers
// panel is one revision and the brush takes it; a stroke on the picture paints the layer mask and is one revision once
// the brush rests, Ctrl+Z takes it back; Backslash toggles the red overlay and Ctrl+I inverts; Q paints the selection
// (Quick Mask), and Use selection hands it to the followed Canvas Region as one undo step.

#include <QDir>
#include <QQmlContext>
#include <QQmlProperty>
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
    for (int i = 0; still < 5 && i < 500; ++i) {
        QTest::qWait(10);
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

// Whether `item`'s tooltip is showing: the attached ToolTip.visible, which the shared popup follows.
bool tipShown(QQuickItem *item) { return QQmlProperty(item, "ToolTip.visible", qmlContext(item)).read().toBool(); }

QByteArray withRegion(const QString &canvas) {
    return QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {"region": {"type": "fugue/canvas-region", "version": "1.2.0", "inputs": {"canvas": "%1"}}}},
  "layout": {"nodes": {"region": {"pos": [20, 20]}}, "view": {"center": [200, 200], "zoom": 1}}})").arg(canvas).toUtf8();
}

} // namespace

class TestCanvasToolsWindow : public QObject {
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
        QTest::mouseMove(app.window, at);
        [&] { QTRY_VERIFY(view(app)->property("keysHot").toBool()); }();
        return at;
    }
    void key(App &app, int k, Qt::KeyboardModifiers mods = {}) { QTest::keyClick(app.window, Qt::Key(k), mods); }
    // A drag across the picture from its center, `dx` pixels to the right.
    void drag(App &app, QPoint from, int dx) {
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        for (int x = 4; x <= dx; x += 4)
            QTest::mouseMove(app.window, from + QPoint(x, 0));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, from + QPoint(dx, 0));
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
        d.loadLayer(LayerKind::Paint, "Fill", true, 1, solid(256, 256, {205, 195, 160, 255})); // red marks read on it
        const auto made = canvas::CanvasStore::forSession().create("Tools", d, "new");
        QVERIFY(made);
        m_canvas = made->id;
        m_head = made->head;
    }

    // The strip: the region is the default tool; each tool has its key in its tooltip and takes it while the pointer
    // is over the view; over the layers panel the panel's keys win (X deletes a layer there, swaps the brush here).
    void theStripsToolsAndTheirKeys() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QCOMPARE(view(app)->property("tool").toString(), QString("region"));
        QVERIFY(find(app.window, "canvasViewTool-region")->property("current").toBool());
        QVERIFY(!find(app.window, "canvasViewTool-region")->property("marked").toBool()); // the quiet marking (D7)
        QVERIFY(find(app.window, "canvasViewTool-brush")->property("tip").toString().endsWith("(B)"));
        QVERIFY(find(app.window, "canvasViewQuickMask")->property("tip").toString().endsWith("(Q)"));
        hoverPicture(app);
        QVERIFY(app.commands.contextActive("canvas-view"));
        key(app, Qt::Key_B);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("brush"));
        QVERIFY(find(app.window, "canvasViewBrushSize"));
        key(app, Qt::Key_X);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("eraser"));
        key(app, Qt::Key_E);
        key(app, Qt::Key_X);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("brush"));
        const double before = view(app)->property("brushSize").toDouble();
        key(app, Qt::Key_BracketRight);
        QTRY_VERIFY(view(app)->property("brushSize").toDouble() > before);
        key(app, Qt::Key_H);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("hand"));
        key(app, Qt::Key_Z);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("zoom"));
        key(app, Qt::Key_M);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("region"));
        click(app, "canvasViewTool-brush");
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("brush"));
        // No target yet: the options bar offers Add mask and Quick Mask.
        QVERIFY(find(app.window, "canvasViewBrushAddMask"));
        QVERIFY(find(app.window, "canvasViewBrushQuickMask"));
        QCOMPARE(revisions(), 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Add mask, paint, rest: one revision; Ctrl+Z over the picture takes it back; \ toggles the overlay; Ctrl+I inverts.
    void addMaskPaintItAndUndoIt() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        model(app)->setProperty("maskIdle", 80);
        click(app, "canvasLayer-1");
        click(app, "canvasLayerAddMask");
        QTRY_COMPARE(revisions(), 2);
        QCOMPARE(model(app)->property("maskTarget").toInt(), 1);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("brush")); // the brush takes the new mask
        QTRY_VERIFY(find(app.window, "canvasViewBrushTarget"));
        QCOMPARE(find(app.window, "canvasViewBrushTargetText")->property("text").toString(), QString("Mask of Fill"));
        QTRY_VERIFY(find(app.window, "canvasLayerMask-1"));
        QCOMPARE(find(app.window, "canvasLayerMask-1")->property("targeted").toBool(), true);
        QVERIFY(find(app.window, "canvasViewLayerMask")); // the overlay, on by default
        const QPoint at = hoverPicture(app);
        drag(app, at, 40);
        QTRY_COMPARE(revisions(), 3);
        QCOMPARE(canvas::CanvasStore::forSession().revision(m_canvas, head())->producer, QString("canvas-view:mask-paint"));
        QTRY_VERIFY(layerMask(app)->valueAt(128, 128) < 128); // the brush hides
        key(app, Qt::Key_Backslash);
        QTRY_VERIFY(!find(app.window, "canvasViewLayerMask"));
        key(app, Qt::Key_Backslash);
        QTRY_VERIFY(find(app.window, "canvasViewLayerMask"));
        key(app, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(revisions(), 2);
        QTRY_COMPARE(layerMask(app)->valueAt(128, 128), 255);
        key(app, Qt::Key_I, Qt::ControlModifier);
        QTRY_COMPARE(revisions(), 3);
        QCOMPARE(layerMask(app)->valueAt(128, 128), 0);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // A Quick Mask stroke is the selection on release (#313): the ants outline it at once (no tint, no revision), strokes add,
    // the eraser (X) subtracts, Ctrl+Z takes one back (also with Q off), Deselect is undoable; Use selection writes the
    // followed Canvas Region (its hash, one undo step of the document); the tint is optional (\\).
    void aQuickMaskStrokeIsTheSelectionOnRelease() {
        App app(load(withRegion(m_canvas)));
        QVERIFY(app.open());
        app.model.select({"region"});
        openDialog(app);
        QTRY_VERIFY(model(app)->property("region").toMap().value("valid").toBool());
        const QPoint at = hoverPicture(app);
        key(app, Qt::Key_Q);
        QTRY_VERIFY(view(app)->property("quickMask").toBool());
        QCOMPARE(view(app)->property("tool").toString(), QString("brush"));
        QCOMPARE(find(app.window, "canvasViewBrushTargetText")->property("text").toString(), QString("Selection"));
        drag(app, at, 40);
        QTRY_VERIFY(selection(app)->valueAt(128, 128) > 128);
        QTRY_VERIFY(find(app.window, "canvasViewAnts") && find(app.window, "canvasViewAnts")->property("outlined").toBool());
        QVERIFY(find(app.window, "canvasViewSelection")); // the red is on while Q is on (#380); \ hides it
        key(app, Qt::Key_Backslash);
        QTRY_VERIFY(!find(app.window, "canvasViewSelection"));
        key(app, Qt::Key_Backslash);
        QTRY_VERIFY(find(app.window, "canvasViewSelection"));
        QCOMPARE(revisions(), 1); // a selection is not a revision
        // A second stroke elsewhere adds: the first stays.
        drag(app, at + QPoint(0, 80), 20);
        QTRY_VERIFY(selection(app)->canUndo());
        QVERIFY(selection(app)->valueAt(128, 128) > 128);
        // X swaps to the eraser: a stroke over the first takes it out.
        key(app, Qt::Key_X);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("eraser"));
        drag(app, at, 40);
        QTRY_COMPARE(selection(app)->valueAt(128, 128), 0);
        key(app, Qt::Key_Z, Qt::ControlModifier); // one stroke back
        QTRY_VERIFY(selection(app)->valueAt(128, 128) > 128);
        // The tint is optional.
        key(app, Qt::Key_Backslash);
        QTRY_VERIFY(!find(app.window, "canvasViewSelection"));
        // Leaving Q keeps the ants, and Ctrl+Z still takes the selection back, one stroke.
        key(app, Qt::Key_Q);
        QTRY_VERIFY(!view(app)->property("quickMask").toBool());
        QVERIFY(find(app.window, "canvasViewAnts"));
        key(app, Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(selection(app)->valueAt(128, 128) > 128 || !view(app)->property("maskSelected").toBool());
        QCOMPARE(revisions(), 1);
        // Deselect, and Ctrl+Z brings it back.
        QVERIFY(app.commands.trigger("canvas-view.select-all"));
        QTRY_VERIFY(view(app)->property("maskSelected").toBool());
        QVERIFY(app.commands.trigger("canvas-view.deselect"));
        QTRY_VERIFY(!view(app)->property("maskSelected").toBool());
        key(app, Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(view(app)->property("maskSelected").toBool());
        // Use selection is on for a painted selection, with Q off.
        QVERIFY(app.commands.entry("canvas-view.use-selection").value("enabled").toBool());
        QVERIFY(app.commands.trigger("canvas-view.use-selection"));
        const QString hash = app.model.widgets()->regionState("region").value("selection").toString();
        QVERIFY(hash.startsWith("sha256:"));
        QVERIFY(canvas::CanvasStore::forSession().selection(hash).ok());
        QCOMPARE(model(app)->property("region").toMap().value("selection").toString(), hash);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The flow of the old quickMaskPaintsTheSelectionForTheRegion (crashed on main after the ants, #313): Q, stroke, Ctrl+Z (the
    // mask empties), stroke, Use selection, Q off.
    void quickMaskPaintsTheSelectionForTheRegion() {
        App app(load(withRegion(m_canvas)));
        QVERIFY(app.open());
        app.model.select({"region"});
        openDialog(app);
        QTRY_VERIFY(model(app)->property("region").toMap().value("valid").toBool());
        const QPoint at = hoverPicture(app);
        key(app, Qt::Key_Q);
        QTRY_VERIFY(view(app)->property("quickMask").toBool());
        QCOMPARE(find(app.window, "canvasViewBrushTargetText")->property("text").toString(), QString("Selection"));
        drag(app, at, 40);
        QTRY_VERIFY(selection(app)->valueAt(128, 128) > 128);
        QCOMPARE(revisions(), 1); // a selection is not a revision
        key(app, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(selection(app)->valueAt(128, 128), 0);
        drag(app, at, 40);
        QTRY_VERIFY(selection(app)->valueAt(128, 128) > 128);
        const int steps = app.model.undoStack()->count();
        click(app, "canvasViewUseSelection");
        const QString hash = app.model.widgets()->regionState("region").value("selection").toString();
        QVERIFY(hash.startsWith("sha256:"));
        QCOMPARE(app.model.undoStack()->count(), steps + 1);
        QVERIFY(canvas::CanvasStore::forSession().selection(hash).ok());
        QCOMPARE(model(app)->property("region").toMap().value("selection").toString(), hash);
        key(app, Qt::Key_Q);
        QTRY_VERIFY(!view(app)->property("quickMask").toBool());
        QVERIFY(!find(app.window, "canvasViewSelection")); // no tint with Quick Mask off
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // A rectangle is the start of the painted selection: the first stroke joins it; a faint stroke (no 50% contour) shows the tint.
    void aRectangleJoinsTheStrokeAndAFaintStrokeShowsTheTint() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        const QPoint at = hoverPicture(app);
        const QVariant rect = QVariantMap{{"x", 20}, {"y", 30}, {"width", 40}, {"height", 40}};
        QVERIFY(QMetaObject::invokeMethod(view(app), "placeFree", Q_ARG(QVariant, rect)));
        QTRY_VERIFY(view(app)->property("hasFreeRect").toBool());
        key(app, Qt::Key_Q);
        QTRY_VERIFY(view(app)->property("quickMask").toBool());
        drag(app, at, 40);
        QTRY_VERIFY(selection(app)->valueAt(128, 128) > 128);
        QCOMPARE(selection(app)->valueAt(40, 50), 255); // the rectangle, painted in first
        QVERIFY(!view(app)->property("hasFreeRect").toBool());
        QCOMPARE(revisions(), 1);
        // Faint: nothing for the ants to outline, so the tint shows by itself.
        QVERIFY(app.commands.trigger("canvas-view.deselect"));
        QTRY_VERIFY(!view(app)->property("maskSelected").toBool());
        view(app)->setProperty("brushOpacity", 0.3);
        drag(app, at, 40);
        QTRY_VERIFY(view(app)->property("maskSelected").toBool());
        QTRY_VERIFY(find(app.window, "canvasViewSelection"));
        QVERIFY(!find(app.window, "canvasViewAnts") || !find(app.window, "canvasViewAnts")->property("outlined").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // A layer-mask stroke is one revision and leaves the selection alone; a Quick Mask stroke is no revision.
    void aLayerMaskStrokeLeavesTheSelectionAlone() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        const QPoint at = hoverPicture(app);
        key(app, Qt::Key_Q);
        QTRY_VERIFY(view(app)->property("quickMask").toBool());
        drag(app, at + QPoint(0, 60), 20);
        QTRY_VERIFY(view(app)->property("maskSelected").toBool());
        const int before = selection(app)->valueAt(128, 128);
        key(app, Qt::Key_Q);
        QTRY_VERIFY(!view(app)->property("quickMask").toBool());
        model(app)->setProperty("maskIdle", 80);
        click(app, "canvasLayer-1");
        click(app, "canvasLayerAddMask");
        QTRY_COMPARE(revisions(), 2);
        drag(app, at, 40);
        QTRY_COMPARE(revisions(), 3);
        QCOMPARE(selection(app)->valueAt(128, 128), before);
        QVERIFY(view(app)->property("maskSelected").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The Selection Brush (#380): A picks it (its tooltip names the key), its stroke is the selection with no Quick Mask and no revision,
    // the layer mask under a targeted layer stays untouched, and Alt at the press subtracts (the size is [ and ], Alt does not size it).
    void theSelectionBrushPaintsTheSelectionNotTheMask() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        model(app)->setProperty("maskIdle", 80);
        click(app, "canvasLayer-1");
        click(app, "canvasLayerAddMask");
        QTRY_COMPARE(revisions(), 2);
        QVERIFY(find(app.window, "canvasViewTool-selbrush")->property("tip").toString().endsWith("(A)"));
        const QPoint at = hoverPicture(app);
        key(app, Qt::Key_A);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("selbrush"));
        QVERIFY(!view(app)->property("quickMask").toBool());
        QCOMPARE(find(app.window, "canvasViewBrushTargetText")->property("text").toString(), QString("Selection"));
        drag(app, at, 40);
        QTRY_VERIFY(selection(app)->valueAt(128, 128) > 128);
        QTRY_VERIFY(find(app.window, "canvasViewAnts") && find(app.window, "canvasViewAnts")->property("outlined").toBool());
        QVERIFY(!find(app.window, "canvasViewSelection")); // the red was the live trace: the ants took over on release
        QCOMPARE(layerMask(app)->valueAt(128, 128), 255); // the mask is untouched
        QCOMPARE(revisions(), 2);
        const double size = view(app)->property("brushSize").toDouble();
        QTest::mousePress(app.window, Qt::LeftButton, Qt::AltModifier, at);
        QTest::mouseMove(app.window, at + QPoint(10, 0));
        QTest::mouseRelease(app.window, Qt::LeftButton, Qt::AltModifier, at + QPoint(10, 0));
        QTRY_COMPARE(selection(app)->valueAt(128, 128), 0); // Alt subtracted
        QCOMPARE(view(app)->property("brushSize").toDouble(), size);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The lasso (#230): L picks it, a freehand drag around the picture's center selects it (the mask Quick Mask paints, no revision),
    // Shift adds, Alt subtracts, Ctrl+Z takes it back, Escape drops a drag; Select > Inverse and Deselect work on it.
    void theLassoSelectsFreehandAndTheSelectMenuWorksOnIt() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QVERIFY(find(app.window, "canvasViewTool-lasso")->property("tip").toString().endsWith("(L)"));
        const QPoint at = hoverPicture(app);
        key(app, Qt::Key_L);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("lasso"));
        const auto lasso = [&](int half, Qt::KeyboardModifiers mods, QPoint center) {
            const QPoint corners[] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
            QTest::mousePress(app.window, Qt::LeftButton, mods, center + corners[0]);
            for (int i = 1; i <= 4; ++i) {
                const QPoint a = center + corners[i - 1], b = center + corners[i % 4];
                for (int t = 1; t <= 4; ++t)
                    QTest::mouseMove(app.window, a + (b - a) * t / 4);
            }
            QTest::mouseRelease(app.window, Qt::LeftButton, mods, center + corners[0]);
        };
        lasso(20, {}, at);
        QTRY_COMPARE(selection(app)->valueAt(128, 128), 255);
        QCOMPARE(revisions(), 1); // a selection is not a revision
        QVERIFY(view(app)->property("maskSelected").toBool());
        lasso(8, Qt::AltModifier, at); // Alt takes a hole out of it
        QTRY_COMPARE(selection(app)->valueAt(128, 128), 0);
        key(app, Qt::Key_Z, Qt::ControlModifier); // one step back: the selection, not the canvas
        QTRY_COMPARE(selection(app)->valueAt(128, 128), 255);
        QCOMPARE(revisions(), 1);
        QVERIFY(app.commands.trigger("canvas-view.invert-selection"));
        QTRY_COMPARE(selection(app)->valueAt(128, 128), 0);
        QVERIFY(app.commands.trigger("canvas-view.deselect"));
        QTRY_VERIFY(!view(app)->property("maskSelected").toBool());
        QTest::mousePress(app.window, Qt::LeftButton, {}, at); // Escape drops a drag in progress
        QTest::mouseMove(app.window, at + QPoint(30, 0));
        QTest::mouseMove(app.window, at + QPoint(30, 30));
        key(app, Qt::Key_Escape);
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, at + QPoint(30, 30));
        QVERIFY(!view(app)->property("maskSelected").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The marching ants (#313): a rectangle (the free one the region tool draws) and a lasso selection have an outlined ants item,
    // Quick Mask keeps it (the red tint is optional), a layer mask made the selection (Ctrl+click its thumbnail) has one, and
    // Deselect (Ctrl+D) removes it.
    void theSelectionHasMarchingAnts() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        const auto ants = [&] { return find(app.window, "canvasViewAnts"); };
        QVERIFY(!ants());
        const QPoint at = hoverPicture(app);
        const QPoint from = at - QPoint(40, 40);
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        for (int i = 4; i <= 60; i += 4)
            QTest::mouseMove(app.window, from + QPoint(i, i));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, from + QPoint(60, 60));
        QTRY_VERIFY(ants() && ants()->property("outlined").toBool());
        QCOMPARE(ants()->property("pathCount").toInt(), 1);
        QVERIFY(app.commands.trigger("canvas-view.deselect"));
        QTRY_VERIFY(!ants());
        key(app, Qt::Key_L);
        QTRY_COMPARE(view(app)->property("tool").toString(), QString("lasso"));
        const QPoint corners[] = {{-20, -20}, {20, -20}, {20, 20}, {-20, 20}};
        QTest::mousePress(app.window, Qt::LeftButton, {}, at + corners[0]);
        for (int i = 1; i <= 4; ++i)
            for (int t = 1; t <= 4; ++t)
                QTest::mouseMove(app.window, at + corners[i - 1] + (corners[i % 4] - corners[i - 1]) * t / 4);
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, at + corners[0]);
        QTRY_VERIFY(ants() && ants()->property("outlined").toBool());
        QVERIFY(ants()->property("pointCount").toInt() < 2000);
        const int before = ants()->property("pointCount").toInt();
        QVERIFY(app.commands.trigger("canvas-view.invert-selection")); // the same edge and the picture's own
        QTRY_VERIFY(ants()->property("pointCount").toInt() != before);
        key(app, Qt::Key_Q); // Quick Mask edits it, the ants stay (the tint is optional)
        QTRY_VERIFY(view(app)->property("quickMask").toBool());
        QVERIFY(ants());
        key(app, Qt::Key_Q);
        QTRY_VERIFY(ants());
        key(app, Qt::Key_D, Qt::ControlModifier);
        QTRY_VERIFY(!ants());
        // A layer mask as the selection.
        click(app, "canvasLayer-1");
        click(app, "canvasLayerAddMask");
        QTRY_VERIFY(find(app.window, "canvasLayerMask-1"));
        click(app, "canvasLayerMask-1", Qt::ControlModifier);
        QTRY_VERIFY(ants() && ants()->property("outlined").toBool());
        QVERIFY(app.commands.trigger("canvas-view.deselect"));
        QTRY_VERIFY(!ants());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Pictures: FUGUE_SHOT_DIR=<dir> test_canvas_tools_window savesThePicturesOfTheAnts (nothing without the variable).
    void savesThePicturesOfTheAnts() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("FUGUE_SHOT_DIR is not set");
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"), {}, Look{"tokyo-night", 8});
        QVERIFY(app.open());
        openDialog(app);
        const auto save = [&](const QString &what) {
            QTest::mouseMove(app.window, QPoint(2, 2)); // the pointer off the picture
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/ants-%2.png").arg(QString::fromLocal8Bit(dir), what)));
        };
        const QPoint at = hoverPicture(app);
        const QPoint from = at - QPoint(60, 50);
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        for (int i = 4; i <= 100; i += 4)
            QTest::mouseMove(app.window, from + QPoint(i, i * 3 / 4));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, from + QPoint(100, 75));
        QTRY_VERIFY(find(app.window, "canvasViewAnts"));
        save("rect");
        QVERIFY(app.commands.trigger("canvas-view.deselect"));
        key(app, Qt::Key_L);
        const QPoint ring[] = {{0, -45}, {35, -30}, {50, 5}, {30, 40}, {-5, 50}, {-40, 30}, {-50, -10}, {-30, -40}};
        QTest::mousePress(app.window, Qt::LeftButton, {}, at + ring[0]);
        for (int i = 1; i <= 8; ++i)
            for (int t = 1; t <= 4; ++t)
                QTest::mouseMove(app.window, at + ring[i - 1] + (ring[i % 8] - ring[i - 1]) * t / 4);
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, at + ring[0]);
        QTRY_VERIFY(find(app.window, "canvasViewAnts"));
        save("lasso");
    }

    // Picture of a brush-painted selection: FUGUE_SHOT_DIR=<dir> test_canvas_tools_window savesThePictureOfABrushSelection.
    void savesThePictureOfABrushSelection() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("FUGUE_SHOT_DIR is not set");
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"), {}, Look{"tokyo-night", 8});
        QVERIFY(app.open());
        openDialog(app);
        const QPoint at = hoverPicture(app);
        key(app, Qt::Key_Q);
        QTRY_VERIFY(view(app)->property("quickMask").toBool());
        view(app)->setProperty("brushSize", 36);
        for (const QPoint &p : {QPoint(-50, -20), QPoint(-50, 10), QPoint(-20, 30)}) {
            QTest::mousePress(app.window, Qt::LeftButton, {}, at + p);
            for (int x = 4; x <= 90; x += 4)
                QTest::mouseMove(app.window, at + p + QPoint(x, x / 6));
            QTest::mouseRelease(app.window, Qt::LeftButton, {}, at + p + QPoint(90, 15));
        }
        QTRY_VERIFY(find(app.window, "canvasViewAnts") && find(app.window, "canvasViewAnts")->property("outlined").toBool());
        QTest::mouseMove(app.window, QPoint(2, 2));
        QTest::qWait(400);
        QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/brush-selection.png").arg(QString::fromLocal8Bit(dir))));
    }

    // #281: Qt gives a disabled item no hover, so a disabled button's "why is this off" tip never showed. Use selection is
    // off with no Canvas Region node in the graph; resting the pointer on it shows its tip, a click on it does nothing,
    // and leaving it hides the tip.
    void aDisabledButtonShowsItsTip() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        hoverPicture(app);
        key(app, Qt::Key_Q);
        QTRY_VERIFY(view(app)->property("quickMask").toBool());
        QQuickItem *button = find(app.window, "canvasViewUseSelection");
        QVERIFY(button);
        QVERIFY(!button->isEnabled());
        const int steps = app.model.undoStack()->count();
        const QPoint at = settled(app.window, "canvasViewUseSelection");
        QVERIFY(at.x() >= 0);
        QTest::mouseMove(app.window, at - QPoint(60, 0));
        QVERIFY(!tipShown(button));
        QTest::mouseMove(app.window, at);
        QTRY_VERIFY(tipShown(button));
        QCOMPARE(button->property("tip").toString(), QString("Add or select a Canvas Region node first: it is what a run redraws"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, at);
        QCOMPARE(app.model.undoStack()->count(), steps); // the click did nothing
        QTest::mouseMove(app.window, at - QPoint(60, 0));
        QTRY_VERIFY(!tipShown(button));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Pictures: FUGUE_SHOT_DIR=<dir> test_canvas_tools_window savesThePicturesOfTheTools (nothing without the variable),
    // dark and light: the strip and the brush's options with a layer mask painted, Quick Mask, and a narrow view.
    void savesThePicturesOfTheTools_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("name");
        QTest::newRow("dark") << "tokyo-night" << "dark";
        QTest::newRow("light") << "catppuccin-latte" << "light";
    }
    void savesThePicturesOfTheTools() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("FUGUE_SHOT_DIR is not set");
        QFETCH(QString, theme);
        QFETCH(QString, name);
        const auto save = [&](App &app, const QString &what) {
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/canvas-tools-%2-%3.png").arg(QString::fromLocal8Bit(dir), name, what)));
        };
        App app(load(withRegion(m_canvas)), {}, Look{theme, 8});
        QVERIFY(app.open());
        openDialog(app);
        model(app)->setProperty("maskIdle", 50);
        save(app, "region");
        click(app, "canvasLayer-1");
        click(app, "canvasLayerAddMask");
        QTRY_COMPARE(revisions(), 2);
        const QPoint at = hoverPicture(app);
        drag(app, at, 60);
        QTRY_COMPARE(revisions(), 3);
        save(app, "mask");
        key(app, Qt::Key_Q);
        QTRY_VERIFY(view(app)->property("quickMask").toBool());
        drag(app, at - QPoint(40, 40), 30);
        save(app, "quick-mask");
        app.window->resize(560, 520);
        save(app, "narrow");
    }
};

QTEST_MAIN(TestCanvasToolsWindow)
#include "test_canvas_tools_window.moc"
