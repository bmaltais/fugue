// The layers panel of the Canvas view in the real window (#226, spec 14.5), offscreen, driven by QTest events only (no
// real window, no real pointer): File > Canvas… lists the head's layers top first; the eye hides a layer as one
// revision and Alt+click shows it alone without one; Opacity is a drag-number field (drag, double-click resets); Blend is
// a menu; a double click renames; Ctrl and Shift select several; the keys work while the pointer is over the panel (H,
// Delete, Ctrl+D, ], Ctrl+Z); a right click opens the same operations with their keys; a drag reorders; Shift+click on
// the mask turns it off; an older revision reads only.

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

// Where an item is once the layout has settled (a list is rebuilt when the model changes).
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

} // namespace

class TestCanvasLayersWindow : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_canvas, m_head;

    QObject *dialog(App &app) const { return app.window->findChild<QObject *>("canvasViewDialog"); }
    QObject *view(App &app) const { return dialog(app)->property("view").value<QObject *>(); }
    QObject *model(App &app) const { return view(app)->property("backend").value<QObject *>(); }
    QObject *panel(App &app) const { return view(app)->property("layersPanel").value<QObject *>(); }
    QVariantList layers(App &app) const { return model(app)->property("layers").toList(); }
    QVariantMap layer(App &app, int position) const {
        for (const QVariant &row : layers(app))
            if (row.toMap().value("position").toInt() == position)
                return row.toMap();
        return {};
    }
    int revisions() const { return int(canvas::CanvasStore::forSession().log(m_canvas).value().size()); }
    void openDialog(App &app) {
        QVERIFY(app.commands.trigger("file.canvas-view"));
        QTRY_VERIFY(dialog(app) && dialog(app)->property("opened").toBool());
        QTRY_VERIFY(find(app.window, "canvasLayer-2"));
    }
    void click(App &app, const QString &name, Qt::KeyboardModifiers mods = {}, Qt::MouseButton button = Qt::LeftButton) {
        const QPoint at = settled(app.window, name);
        QVERIFY2(at.x() >= 0, qPrintable(name + " never settled"));
        QTest::mouseClick(app.window, button, mods, at);
    }
    // The menu the panel opened, and the entry with this text chosen as a click on its row does.
    QObject *menu(App &app) const { return panel(app)->findChild<QObject *>("widgetMenu"); }
    void choose(App &app, const QString &text) {
        QTRY_VERIFY(menu(app) && menu(app)->property("opened").toBool());
        const QVariantList shown = menu(app)->property("shown").toList();
        for (int i = 0; i < shown.size(); ++i)
            if (shown[i].toMap().value("text").toString() == text) {
                QVERIFY(QMetaObject::invokeMethod(menu(app), "choose", Q_ARG(QVariant, i)));
                return;
            }
        QFAIL(qPrintable("no entry " + text));
    }
    // The pointer (QTest's, never the real one) over the panel: its keys are live.
    void hoverPanel(App &app) {
        const QPoint at = settled(app.window, "canvasLayerList");
        QVERIFY(at.x() >= 0);
        QTest::mouseMove(app.window, at);
        QTRY_VERIFY(panel(app)->property("hot").toBool());
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
        const auto made = canvas::CanvasStore::forSession().create("Layers", d, "new");
        QVERIFY(made);
        m_canvas = made->id;
        m_head = made->head;
    }

    void theEyeHidesAsOneRevisionAndAltClickShowsALayerAlone() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QVERIFY(find(app.window, "canvasLayerThumb-1"));
        QVERIFY(find(app.window, "canvasLayerMask-1"));
        QVERIFY(!find(app.window, "canvasLayerMask-0"));
        QVERIFY(find(app.window, "canvasLayerOpacity")); // simple first: eye, opacity, delete in view
        QVERIFY(find(app.window, "canvasLayerDelete"));
        click(app, "canvasLayerEye-0");
        QTRY_VERIFY(!layer(app, 0).value("visible").toBool());
        QCOMPARE(revisions(), 2);
        click(app, "canvasLayerEye-1", Qt::AltModifier);
        QTRY_COMPARE(model(app)->property("solo").toInt(), 1);
        QVERIFY(find(app.window, "canvasLayersSolo"));
        QCOMPARE(revisions(), 2); // a view, not a revision
        click(app, "canvasLayersSoloEnd");
        QTRY_COMPARE(model(app)->property("solo").toInt(), -1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #226: the lock toggle is one revision that disables Delete for the layer; Generate reading a layer shows a notice, no revision.
    void theLockProtectsALayerAndGenerateCanReadOneLayerOnly() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        click(app, "canvasLayer-1");
        QTRY_COMPARE(model(app)->property("selection").toList(), (QVariantList{1}));
        QVERIFY(find(app.window, "canvasLayerDelete")->isEnabled());
        click(app, "canvasLayerLock-1");
        QTRY_VERIFY(layer(app, 1).value("locked").toBool());
        QCOMPARE(revisions(), 2);
        QTRY_VERIFY(!find(app.window, "canvasLayerDelete")->isEnabled());
        QVERIFY(!find(app.window, "canvasLayersRegion"));
        QVERIFY(QMetaObject::invokeMethod(model(app), "setRegionLayer", Q_ARG(int, 1)));
        QTRY_VERIFY(find(app.window, "canvasLayersRegion"));
        QCOMPARE(revisions(), 2); // a view state
        click(app, "canvasLayersRegionEnd");
        QTRY_VERIFY(!find(app.window, "canvasLayersRegion"));
        click(app, "canvasLayerLock-1");
        QTRY_VERIFY(!layer(app, 1).value("locked").toBool());
        QTRY_VERIFY(find(app.window, "canvasLayerDelete")->isEnabled());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void opacityIsADragNumberFieldAndBlendAMenu() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        click(app, "canvasLayer-1");
        QTRY_COMPARE(model(app)->property("selection").toList(), (QVariantList{1}));
        // Drag left: the value follows the pointer and one revision is made on release.
        const QPoint at = settled(app.window, "canvasLayerOpacity");
        QTest::mousePress(app.window, Qt::LeftButton, {}, at);
        for (int dx = 4; dx <= 60; dx += 4)
            QTest::mouseMove(app.window, at - QPoint(dx, 0));
        QCOMPARE(revisions(), 1);
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, at - QPoint(60, 0));
        QTRY_VERIFY(layer(app, 1).value("opacity").toDouble() < 0.8);
        QCOMPARE(revisions(), 2);
        // A double click puts 100% back.
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, settled(app.window, "canvasLayerOpacity"));
        QTRY_COMPARE(layer(app, 1).value("opacity").toDouble(), 1.0);
        QCOMPARE(revisions(), 3);

        click(app, "canvasLayerBlend");
        choose(app, "Multiply");
        QTRY_COMPARE(layer(app, 1).value("blend").toString(), QString("multiply"));
        QCOMPARE(revisions(), 4);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aDoubleClickRenamesAndCtrlAndShiftSelectSeveral() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, settled(app.window, "canvasLayerName-2"));
        QTRY_VERIFY(find(app.window, "canvasLayerRename"));
        QTest::keyClick(app.window, Qt::Key_A, Qt::ControlModifier);
        for (char c : QByteArray("Sky"))
            QTest::keyClick(app.window, c);
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_COMPARE(layer(app, 2).value("name").toString(), QString("Sky"));
        QCOMPARE(revisions(), 2);

        click(app, "canvasLayer-0");
        click(app, "canvasLayer-2", Qt::ControlModifier);
        QTRY_COMPARE(model(app)->property("selection").toList(), (QVariantList{0, 2}));
        click(app, "canvasLayer-0");
        click(app, "canvasLayer-2", Qt::ShiftModifier);
        QTRY_COMPARE(model(app)->property("selection").toList().size(), 3);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theKeysWorkWhileThePointerIsOverThePanel() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        click(app, "canvasLayer-1");
        hoverPanel(app);
        QVERIFY(app.commands.contextActive("canvas-layers"));
        QTest::keyClick(app.window, Qt::Key_H);
        QTRY_VERIFY(!layer(app, 1).value("visible").toBool());
        QTest::keyClick(app.window, Qt::Key_D, Qt::ControlModifier);
        QTRY_COMPARE(layers(app).size(), 4);
        QCOMPARE(layer(app, 2).value("name").toString(), QString("Fill 1 copy"));
        QTest::keyClick(app.window, Qt::Key_BracketRight);
        QTRY_COMPARE(layer(app, 3).value("name").toString(), QString("Fill 1 copy"));
        QTest::keyClick(app.window, Qt::Key_Delete);
        QTRY_COMPARE(layers(app).size(), 3);
        QCOMPARE(revisions(), 5);
        QTest::keyClick(app.window, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(layers(app).size(), 4);
        QTest::keyClick(app.window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(layers(app).size(), 3);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aRightClickOpensTheOperationsWithTheirKeys() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        click(app, "canvasLayer-1", {}, Qt::RightButton);
        QTRY_VERIFY(menu(app) && menu(app)->property("opened").toBool());
        QStringList texts;
        QString deleteKeys;
        for (const QVariant &e : menu(app)->property("entries").toList()) {
            texts << e.toMap().value("text").toString();
            if (e.toMap().value("value").toString() == "delete")
                deleteKeys = e.toMap().value("detail").toString();
        }
        QVERIFY(texts.contains("Hide"));
        QVERIFY(texts.contains("Turn the mask off"));
        QCOMPARE(deleteKeys, QString("X, Del"));
        QVERIFY(!texts.last().startsWith("Delete")); // no destructive row is the last
        choose(app, "Turn the mask off");
        QTRY_VERIFY(!layer(app, 1).value("maskEnabled").toBool());
        // Shift+click on the mask turns it on again.
        click(app, "canvasLayerMask-1", Qt::ShiftModifier);
        QTRY_VERIFY(layer(app, 1).value("maskEnabled").toBool());
        QCOMPARE(revisions(), 3);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aDragReordersWithADropLine() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        const QPoint from = settled(app.window, "canvasLayer-0"); // Base, the bottom row
        const QPoint top = settled(app.window, "canvasLayer-2");
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        for (int y = from.y(); y >= top.y() - 20; y -= 4)
            QTest::mouseMove(app.window, QPoint(from.x(), y));
        QTRY_VERIFY(find(app.window, "canvasLayerDropLine"));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, QPoint(from.x(), top.y() - 20));
        QTRY_COMPARE(layer(app, 2).value("name").toString(), QString("Base"));
        QCOMPARE(revisions(), 2);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #278: the arrow opens a group in the list, its layers sit indented under it, and the eye of one hides that layer alone.
    void anArrowOpensAGroupAndALayerInsideIsHiddenInPlace() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QVERIFY(QMetaObject::invokeMethod(model(app), "groupLayers", Q_ARG(QVariantList, (QVariantList{1, 2}))));
        QTRY_VERIFY(find(app.window, "canvasLayerArrow-1"));
        QVERIFY(!find(app.window, "canvasLayer-2")); // closed: only the group and Base
        click(app, "canvasLayerArrow-1");
        QTRY_COMPARE(layers(app).size(), 4);
        QTRY_VERIFY(find(app.window, "canvasLayerEye-2"));
        QCOMPARE(layer(app, 2).value("depth").toInt(), 1);
        const int before = revisions();
        click(app, "canvasLayerEye-2");
        QTRY_COMPARE(revisions(), before + 1);
        QVERIFY(!layer(app, 2).value("visible").toBool());
        QCOMPARE(layers(app).size(), 4); // the group stayed open through the edit
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void anOlderRevisionReadsOnly() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        click(app, "canvasLayerEye-0");
        QTRY_COMPARE(revisions(), 2);
        QVERIFY(QMetaObject::invokeMethod(model(app), "show", Q_ARG(QString, m_head)));
        QTRY_VERIFY(find(app.window, "canvasLayersReadOnly"));
        QCOMPARE(find(app.window, "canvasLayersReadOnlyText")->property("text").toString(),
                 QString("You are looking at an earlier step. Go back to the latest version to change layers."));
        QVERIFY(!find(app.window, "canvasLayersAccept")); // nothing to decide on an earlier revision
        QVERIFY(!find(app.window, "canvasLayerDelete")->isEnabled());
        click(app, "canvasLayerEye-1");
        QTest::qWait(100);
        QCOMPARE(revisions(), 2);
        // The list still selects (a view-only state), and Go to Head is the way to change it (#228, the owner's report).
        click(app, "canvasLayer-1");
        QTRY_VERIFY(model(app)->property("selection").toList().contains(1));
        click(app, "canvasLayersGoHead");
        QTRY_VERIFY(!find(app.window, "canvasLayersReadOnly"));
        QTRY_VERIFY(find(app.window, "canvasLayerDelete")->isEnabled());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #239: a revision kept apart (made by the command line) shown: the panel says it is not the latest and offers only Back
    // to latest; once there, its layers can be changed. No Accept or Drop.
    void aCandidateShownOffersOnlyBackToLatest() {
        PaintDocument d(256, 256);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(256, 256, {30, 60, 200, 255}));
        d.loadLayer(LayerKind::Paint, "Candidate fill", true, 1, solid(256, 256, {200, 200, 10, 255}));
        const QString candidate = canvas::CanvasStore::forSession().addRevision(m_canvas, d, m_head, "canvas-write:write").value().id;
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QVERIFY(QMetaObject::invokeMethod(model(app), "show", Q_ARG(QString, candidate)));
        QTRY_COMPARE(model(app)->property("revisionB").toString(), candidate);
        QTRY_VERIFY(find(app.window, "canvasLayersReadOnly"));
        QCOMPARE(find(app.window, "canvasLayersReadOnlyText")->property("text").toString(),
                 QString("You are looking at an earlier step. Go back to the latest version to change layers."));
        QVERIFY(find(app.window, "canvasLayersGoHead"));
        QVERIFY(!find(app.window, "canvasLayersAccept") && !find(app.window, "canvasLayersDrop"));
        click(app, "canvasLayersGoHead");
        QTRY_VERIFY(!find(app.window, "canvasLayersReadOnly"));
        QTRY_VERIFY(find(app.window, "canvasLayerDelete")->isEnabled());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Pictures: FUGUE_SHOT_DIR=<dir> test_canvas_layers_window savesThePictureOfThePanel (nothing without the variable).
    void savesThePictureOfThePanel() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("FUGUE_SHOT_DIR is not set");
        // A layer's rename field, dark and light (the kit's text field over the panel's rows), before the tree below groups the layers.
        for (const auto &[theme, tag] : {std::pair{"catppuccin", "dark"}, std::pair{"catppuccin-latte", "light"}}) {
            App themed(load(R"({"fugue": 1, "graph": {"nodes": {}}})"), {}, Look{theme, 8});
            QVERIFY(themed.open());
            openDialog(themed);
            QTest::mouseDClick(themed.window, Qt::LeftButton, {}, settled(themed.window, "canvasLayerName-2"));
            QTRY_VERIFY(find(themed.window, "canvasLayerRename"));
            QTest::qWait(300);
            QVERIFY(test_support::grab(themed.window).save(QString("%1/canvas-layers-rename-%2.png").arg(QString::fromLocal8Bit(dir), tag)));
        }
        {
            App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
            QVERIFY(app.open());
            openDialog(app);
            click(app, "canvasLayer-1");
            click(app, "canvasLayer-0", Qt::ControlModifier);
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/canvas-layers.png"));
            click(app, "canvasLayer-1", {}, Qt::RightButton);
            QTRY_VERIFY(menu(app) && menu(app)->property("opened").toBool());
            QTest::qWait(300);
            QVERIFY(test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/canvas-layers-menu.png"));
            QTest::keyClick(app.window, Qt::Key_Escape);
            QVERIFY(QMetaObject::invokeMethod(model(app), "groupLayers", Q_ARG(QVariantList, (QVariantList{1, 2}))));
            QTRY_VERIFY(find(app.window, "canvasLayerArrow-1"));
            click(app, "canvasLayerArrow-1");
            QTRY_VERIFY(find(app.window, "canvasLayerEye-2"));
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/canvas-layers-tree.png"));
        }
    }
};

FUGUE_TEST_MAIN(TestCanvasLayersWindow, QGuiApplication)
#include "test_canvas_layers_window.moc"
