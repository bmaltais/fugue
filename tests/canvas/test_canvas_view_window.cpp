// The Canvas view in the real window (#221, epics #216/#217, spec 14.5), offscreen: File > Canvas… opens the picture of the
// newest canvas at its head; the History picks a revision, Compare opens the modes, the wheel and keys zoom, and a Canvas Region selected in the graph is
// the follow target: its rectangle is drawn on the picture and "Region reads from this revision" is one undo step. The store
// is the test's own (FUGUE_HOME and the XDG folders point at a temporary folder); no window is launched outside the test
// process and no real pointer is moved.

#include <QDir>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtTest>

#include <thread>

#include "canvas_app.h"
#include "../window_grab.h"
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

// Clicks once the layout has settled (the strip is rebuilt when the model changes).
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

// A command through the view's own menu bar (#232): open the menu by its title and press the row, as a person does.
void viaMenu(App &app, const QString &menu, const QString &command) {
    click(app, "canvasMenuTitle-" + menu);
    click(app, "menuItem-" + command);
}

// The row of a menu, the menu left open (close it with closeMenu).
QQuickItem *menuRow(App &app, const QString &menu, const QString &command) {
    click(app, "canvasMenuTitle-" + menu);
    QQuickItem *row = nullptr;
    for (int i = 0; i < 100 && !row; ++i) {
        row = find(app.window, "menuItem-" + command);
        if (!row)
            QTest::qWait(20);
    }
    return row;
}

// Closes the open menu (and its submenu) by pressing its title again.
void closeMenu(App &app) {
    for (int i = 0; i < 3; ++i) {
        QQuickItem *title = nullptr;
        for (const char *name : {"Canvas", "Edit", "Select", "Layer", "View"})
            if (QQuickItem *row = find(app.window, QString("canvasMenuTitle-") + name); row && row->property("current").toBool())
                title = row;
        if (!title)
            return;
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(title));
        QTest::qWait(60);
    }
}

PaintDocument withFill(PaintDocument doc, const QString &name, int x) {
    doc.addLayer(LayerKind::Paint, name);
    doc.brush.color = Rgba{200, 10, 10, 255};
    doc.fillRect(x, 100, 60, 60);
    return doc;
}

QByteArray fixture(const QString &canvas) {
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {"region": {"type": "fugue/canvas-region", "version": "1.0.0",
                                 "inputs": {"canvas": "%1", "x": 100, "y": 60, "width": 160, "height": 96, "padding": 20}},
                      "text": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "x"}}}},
  "layout": {"nodes": {"region": {"pos": [20, 20]}, "text": {"pos": [20, 200]}}, "view": {"center": [200, 200], "zoom": 1}}
})").arg(canvas).toUtf8();
}

QByteArray linkedFixture(const QString &canvas) {
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {"n": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 40}},
                      "region": {"type": "fugue/canvas-region", "version": "1.0.0",
                                 "inputs": {"canvas": "%1", "x": {"from": "n.value"}, "y": 60, "width": 160, "height": 96}}}},
  "layout": {"nodes": {"n": {"pos": [20, 200]}, "region": {"pos": [20, 20]}}, "view": {"center": [200, 200], "zoom": 1}}
})").arg(canvas).toUtf8();
}

} // namespace

class TestCanvasViewWindow : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_canvas, m_head, m_a, m_b;

    static QString shortOf(const QString &id) { return id.mid(7, 8); }

    QObject *dialog(App &app) const { return app.window->findChild<QObject *>("canvasViewDialog"); }
    QObject *view(App &app) const { return dialog(app)->property("view").value<QObject *>(); }
    QObject *model(App &app) const { return view(app)->property("backend").value<QObject *>(); }
    // Shows a revision as the History panel's click does (the strip of chips is gone, #228).
    void showRevision(App &app, const QString &id) {
        QVERIFY(QMetaObject::invokeMethod(model(app), "show", Q_ARG(QString, id)));
        QTRY_COMPARE(model(app)->property("revisionB").toString(), id);
    }
    void openDialog(App &app) {
        QVERIFY(app.commands.trigger("file.canvas-view"));
        QTRY_VERIFY(dialog(app) && dialog(app)->property("opened").toBool());
        QTRY_VERIFY(find(app.window, "canvasViewViewer"));
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
        QDir(m_dir.filePath("config")).removeRecursively(); // a test's setting does not reach the next
        canvas::CanvasStore store = canvas::CanvasStore::forSession();
        PaintDocument base(512, 512);
        base.addLayer(LayerKind::Paint, "Base");
        base.brush.color = Rgba{30, 60, 200, 255};
        base.fillRect(0, 0, 512, 512);
        const auto made = store.create("Sketch", base, "new");
        QVERIFY(made);
        m_canvas = made->id;
        m_head = made->head;
        m_a = store.addRevision(m_canvas, withFill(base, "Fill A", 100), m_head, "canvas-write:a").value().id;
        m_b = store.addRevision(m_canvas, withFill(base, "Fill B", 300), m_head, "canvas-write:b").value().id;
    }

    void theMenuOpensTheNewestCanvasAtItsHeadWithTheStrip() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_COMPARE(model(app)->property("canvasId").toString(), m_canvas);
        QTRY_VERIFY(!model(app)->property("revisionB").toString().isEmpty());
        // No strip and no hashes (#228), and no review of candidates (#239): no chip for the two kept apart.
        QVERIFY(!find(app.window, "canvasViewChoice-head"));
        QVERIFY(!find(app.window, "canvasViewPending"));
        QVERIFY(!find(app.window, "canvasHistoryPanel")); // closed at first
        QCOMPARE(model(app)->property("revisionB").toString(), m_head);
        // The head alone: Export is there.
        QVERIFY(!find(app.window, "canvasViewAccept"));
        QVERIFY(!find(app.window, "canvasViewOverlay")); // it lives in the View menu now
        QVERIFY(!find(app.window, "canvasViewExport")); // and Export in the File menu
        QTRY_VERIFY(find(app.window, "canvasMenuBar"));
        menuRow(app, "Canvas", "canvas-view.new");
        click(app, "subMenu-Export"); // the submenu opens
        QQuickItem *exportRow = nullptr;
        QTRY_VERIFY((exportRow = find(app.window, "menuItem-canvas-view.export")));
        QVERIFY(exportRow->isEnabled());
        closeMenu(app);
        QVERIFY(!menuRow(app, "View", "canvas-view.show-candidate")); // no candidate toggle in the product (#239)
        closeMenu(app);
        QVERIFY(!find(app.window, "canvasViewRegionRow")); // nothing followed
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #247 and #248: the two export rows are real now, and each opens its dialog on the shown revision.
    void theExportRowsOpenTheirDialogs() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(find(app.window, "canvasMenuBar"));
        const auto exportOpen = [&]() {
            const QObject *box = app.window->findChild<QObject *>("canvasExportDialog");
            return box && box->property("opened").toBool();
        };
        const auto openRow = [&](const QString &command) {
            menuRow(app, "Canvas", "canvas-view.new");
            click(app, "subMenu-Export");
            QQuickItem *row = nullptr;
            QTRY_VERIFY((row = find(app.window, "menuItem-" + command)));
            QVERIFY(row->isEnabled());
            QVERIFY(!find(app.window, "menuPlanned-247-Export with quality and scale…"));
            QVERIFY(!find(app.window, "menuPlanned-248-Export layers as files…"));
            click(app, "menuItem-" + command);
        };
        openRow("canvas-view.export-options");
        QTRY_VERIFY(exportOpen());
        QCOMPARE(find(app.window, "canvasExportTitle")->property("text").toString(), QString("Export with quality and scale"));
        QTRY_VERIFY(find(app.window, "canvasExportEstimate")->property("text").toString().contains("px"));
        QVERIFY(!find(app.window, "canvasExportQuality")); // a PNG has none
        click(app, "canvasExportFormat-jpg");
        QTRY_VERIFY(find(app.window, "canvasExportQuality"));
        QVERIFY(find(app.window, "canvasExportBackground-white"));
        click(app, "canvasExportScale-50");
        QTRY_VERIFY(find(app.window, "canvasExportEstimate")->property("text").toString().contains("px"));
        QVERIFY(find(app.window, "canvasExportOk")->isEnabled());
        click(app, "canvasExportCancel");
        QTRY_VERIFY(!exportOpen());

        openRow("canvas-view.export-layers");
        QTRY_VERIFY(exportOpen());
        QCOMPARE(find(app.window, "canvasExportTitle")->property("text").toString(), QString("Export layers as files"));
        QTRY_VERIFY(find(app.window, "canvasExportFiles"));
        QVERIFY(find(app.window, "canvasExportVisibleOnly"));
        QVERIFY(!find(app.window, "canvasExportFormat-jpg")); // layers keep their transparency
        click(app, "canvasExportCancel");
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #239: a revision kept apart (made by the command line) can be looked at from All revisions: read only, no Accept or Drop.
    void aCandidateShownIsReadOnlyWithBackToLatestAndNoDecision() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        showRevision(app, m_a);
        QTRY_VERIFY(find(app.window, "canvasLayersReadOnly"));
        QVERIFY(find(app.window, "canvasLayersGoHead"));
        QVERIFY(!find(app.window, "canvasViewAccept"));
        QVERIFY(!find(app.window, "canvasViewDrop"));
        QVERIFY(!find(app.window, "canvasLayersAccept"));
        QVERIFY(!find(app.window, "canvasLayersDrop"));
        QVERIFY(!find(app.window, "canvasViewNewResult"));
        click(app, "canvasLayersGoHead");
        QTRY_COMPARE(model(app)->property("revisionB").toString(), m_head);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void compareOpensTheModesAgainstTheHead() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        showRevision(app, m_a);
        QTRY_COMPARE(model(app)->property("revisionB").toString(), m_a);
        QVERIFY(!find(app.window, "canvasViewMode-slider"));
        viaMenu(app, "View", "canvas-view.compare");
        QTRY_VERIFY(find(app.window, "canvasViewMode-slider"));
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        QTRY_COMPARE(viewer->property("mode").toString(), QString("slider"));
        QCOMPARE(viewer->property("sourceA").toUrl(), model(app)->property("sourceA").toUrl());
        QCOMPARE(model(app)->property("revisionA").toString(), m_head);
        click(app, "canvasViewMode-difference");
        QTRY_COMPARE(viewer->property("mode").toString(), QString("difference"));
        viaMenu(app, "View", "canvas-view.compare");
        QTRY_COMPARE(viewer->property("mode").toString(), QString("single"));
    }

    void aStoreChangeWhileOpenShowsWithoutAClick() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(!model(app)->property("revisionB").toString().isEmpty());
        PaintDocument base(512, 512);
        base.addLayer(LayerKind::Paint, "Base");
        const QString c = canvas::CanvasStore::forSession()
                              .addRevision(m_canvas, withFill(base, "Fill C", 200), m_head, "canvas-write:c")
                              .value()
                              .id;
        QTRY_COMPARE_WITH_TIMEOUT(model(app)->property("choices").toList().size(), 4, 4000);
        QVERIFY(c.size() > 0);
    }

    // A candidate is added the way the command line does (addRevision from the head, the head left where it is),
    // from another thread a moment later; the view sees it through the folder watch alone.
    static std::thread arrivesLater(const QString &canvas, const QString &head, const QString &name, int x) {
        return std::thread([=] {
            QThread::msleep(300);
            PaintDocument base(512, 512);
            base.addLayer(LayerKind::Paint, "Base");
            canvas::CanvasStore::forSession().addRevision(canvas, withFill(base, name, x), head, "canvas-write:" + name);
        });
    }

    // #239: a candidate arriving (made apart, by the command line) does not move the view and shows no banner.
    void aCandidateWhileOpenLeavesTheViewOnTheHeadWithNoBanner() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(!model(app)->property("revisionB").toString().isEmpty());
        std::thread run = arrivesLater(m_canvas, m_head, "run1", 200);
        QTRY_VERIFY_WITH_TIMEOUT(model(app)->property("choices").toList().size() > 3, 5000);
        run.join();
        QTest::qWait(300);
        QCOMPARE(model(app)->property("revisionB").toString(), m_head);
        QVERIFY(!find(app.window, "canvasViewNewResult"));
        QVERIFY(!find(app.window, "canvasViewAccept"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The normal flow since #228 (the owner's decision): a run's layer lands on the Head. A view on the Head follows it and
    // says so in a short notice with Undo; no banner, no Accept or Drop.
    void aLayerThatLandsOnTheHeadIsFollowedWithANoticeAndUndo() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(!model(app)->property("revisionB").toString().isEmpty());
        const QString canvas = m_canvas, head = m_head;
        std::thread run([canvas, head] {
            QThread::msleep(300);
            PaintDocument base(512, 512);
            base.addLayer(LayerKind::Paint, "Base");
            canvas::CanvasStore::forSession().commit(canvas, withFill(base, "Fill 9", 200), head, "canvas-write:w");
        });
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "canvasViewHeadToast"), 5000);
        run.join();
        QVERIFY(model(app)->property("head").toString() != m_head);
        QCOMPARE(model(app)->property("revisionB").toString(), model(app)->property("head").toString());
        QCOMPARE(find(app.window, "canvasViewHeadToastText")->property("text").toString(), QString("Layer Fill 9 added"));
        QVERIFY(!find(app.window, "canvasViewNewResult"));
        QVERIFY(!find(app.window, "canvasViewAccept"));
        click(app, "canvasViewHeadToastUndo");
        QTRY_COMPARE(model(app)->property("head").toString(), m_head);
        QTRY_VERIFY(!find(app.window, "canvasViewHeadToast"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theKeysZoomAndTheDragPans() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        QQuickItem *picture = find(app.window, "canvasViewViewer");
        QTRY_VERIFY(picture->width() > 100);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(picture)); // focus the view
        QTest::keyClick(app.window, Qt::Key_1);
        QTRY_VERIFY(viewer->property("actual").toBool());
        // Panned by a middle-button drag at 100% (a left drag draws a selection, #228).
        const qreal before = viewer->property("centerX").toReal();
        const QPoint from = centerOf(picture);
        QTest::mouseMove(app.window, from);
        QTest::mousePress(app.window, Qt::MiddleButton, {}, from);
        QTest::mouseMove(app.window, from + QPoint(40, 0));
        QTest::mouseMove(app.window, from + QPoint(80, 0));
        QTest::mouseRelease(app.window, Qt::MiddleButton, {}, from + QPoint(80, 0));
        QVERIFY(viewer->property("centerX").toReal() < before);
        QTest::keyClick(app.window, Qt::Key_0);
        QTRY_VERIFY(!viewer->property("actual").toBool());
        QCOMPARE(viewer->property("zoom").toReal(), 1.0);
        QTest::keyClick(app.window, Qt::Key_Plus);
        QTRY_VERIFY(viewer->property("zoom").toReal() > 1.0);
    }

    void aSelectedRegionIsDrawnAndReadsFromTheShownRevision() {
        App app(load(fixture(m_canvas)));
        QVERIFY(app.open());
        app.model.select({"region"});
        openDialog(app);
        QTRY_COMPARE(model(app)->property("followNode").toString(), QString("region"));
        QTRY_VERIFY(find(app.window, "canvasViewRegion"));
        QQuickItem *layer = find(app.window, "canvasViewRegion");
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        // The rectangle sits where the numbers say: x 100, y 60, 160 x 96 of the 512 px canvas.
        QQuickItem *frame = find(app.window, "canvasViewFrame");
        QVERIFY(frame);
        const auto frameRect = [frame] { return QRectF(frame->property("fx").toReal(), frame->property("fy").toReal(), frame->property("fw").toReal(), frame->property("fh").toReal()); };
        const QRectF rectangle = frameRect();
        const qreal k = viewer->property("imageScale").toReal();
        QVERIFY(k > 0);
        QVERIFY(qAbs(rectangle.width() - 160 * k) < 0.5);
        QVERIFY(qAbs(rectangle.height() - 96 * k) < 0.5);
        QVERIFY(qAbs(rectangle.x() - (viewer->property("imageX").toReal() + 100 * k)) < 0.5);
        const QRectF context = layer->property("context").toRectF();
        QVERIFY(qAbs(context.width() - 200 * k) < 0.5);
        // It reads the latest version (the normal case): no note and no button under the picture (#234).
        QVERIFY(!find(app.window, "canvasViewUseRegion"));
        QVERIFY(!find(app.window, "canvasViewRegionNote"));
        // Zoom moves the rectangle with the picture.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(find(app.window, "canvasViewViewer")));
        QTest::keyClick(app.window, Qt::Key_1);
        QTRY_VERIFY(qAbs(frameRect().width() - 160) < 0.5);
        // Pinned to a step (a rare choice, written here as the old button did): the note and Use for region come back.
        showRevision(app, m_a);
        QVERIFY(QMetaObject::invokeMethod(model(app), "useForRegion"));
        QCOMPARE(app.model.undoStack()->count(), 1);
        QCOMPARE(app.model.widgets()->regionState("region").value("revision").toString(), m_a);
        QTRY_VERIFY(find(app.window, "canvasViewRegionNote"));
        QVERIFY(find(app.window, "canvasViewRegionNote")->property("text").toString().startsWith("Canvas Region reads “Layer added: Fill A”")); // the step in words, no hash
        QVERIFY(!find(app.window, "canvasViewUseRegion")->isEnabled()); // this step is the one it reads
        // Looking at the latest version: the button points the region back at it, one undo step.
        showRevision(app, m_head);
        QTRY_VERIFY(find(app.window, "canvasViewUseRegion")->isEnabled());
        QCOMPARE(app.model.undoStack()->count(), 1); // looking is no edit
        click(app, "canvasViewUseRegion");
        QCOMPARE(app.model.undoStack()->count(), 2);
        QCOMPARE(app.model.widgets()->regionState("region").value("revision").toString(), QString("head"));
        QTRY_VERIFY(!find(app.window, "canvasViewUseRegion")); // reads the latest again: the row is gone
        app.model.undoStack()->undo();
        QTRY_VERIFY(find(app.window, "canvasViewUseRegion")->isEnabled());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // ---- editing the region on the picture (#224): QTest mouse and key events on the offscreen window only ----

    // Canvas pixel (px, py) as a point of the window, from where the picture is drawn now.
    QPoint pixelAt(App &app, double px, double py) const {
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        const qreal k = viewer->property("imageScale").toReal();
        return static_cast<QQuickItem *>(viewer)
            ->mapToScene(QPointF(viewer->property("imageX").toReal() + px * k, viewer->property("imageY").toReal() + py * k))
            .toPoint();
    }
    // A drag in small steps; `release` false leaves the button down (to look at the live numbers, or press Esc).
    void drag(App &app, QPoint from, QPoint to, bool release = true, Qt::MouseButton button = Qt::LeftButton) {
        QTest::mouseMove(app.window, from);
        QTest::mousePress(app.window, button, {}, from);
        for (int i = 1; i <= 4; ++i)
            QTest::mouseMove(app.window, from + (to - from) * i / 4);
        if (release)
            QTest::mouseRelease(app.window, button, {}, to);
    }
    QVariantMap regionOf(App &app) const { return app.model.widgets()->regionState("region"); }
    void openFollowing(App &app) {
        QVERIFY(app.open());
        app.model.select({"region"});
        openDialog(app);
        QTRY_COMPARE(model(app)->property("followNode").toString(), QString("region"));
        QTRY_VERIFY(find(app.window, "canvasViewFrame"));
        QTest::keyClick(app.window, Qt::Key_0); // Fit
        QTRY_VERIFY(find(app.window, "canvasViewViewer")->width() > 100);
        QTest::qWait(50);
    }
    static bool near(int a, int b, int tolerance = 2) { return qAbs(a - b) <= tolerance; }

    void aHandleResizesTheRegionLiveAndTheReleaseIsOneUndoStep() {
        App app(load(fixture(m_canvas)));
        openFollowing(app);
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        const qreal k = viewer->property("imageScale").toReal();
        // The bottom right corner: x 260, y 156 on the canvas, dragged by 60 x 40 px of the picture.
        const QPoint corner = pixelAt(app, 260, 156);
        const QPoint to = corner + QPoint(qRound(60 * k), qRound(40 * k));
        drag(app, corner, to, false);
        // Live: the numbers, the context box and the model size moved; the node did not.
        QCOMPARE(app.model.undoStack()->count(), 0);
        QCOMPARE(regionOf(app).value("width").toInt(), 160);
        const QString note = find(app.window, "canvasViewStatusSelection")->property("text").toString();
        QVERIFY2(note.contains(QString::number(160 + 60)) || note.contains(QString::number(160 + 59)) || note.contains(QString::number(160 + 61)),
                 qPrintable(note));
        QVERIFY(!find(app.window, "canvasViewRegionInfo")); // the paragraph is gone: a small label and the frame's tooltip
        QVERIFY(find(app.window, "canvasViewContextLabel")->property("text").toString().startsWith("Context "));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, to);
        QCOMPARE(app.model.undoStack()->count(), 1);
        const QVariantMap now = regionOf(app);
        QCOMPARE(now.value("x").toInt(), 100);
        QCOMPARE(now.value("y").toInt(), 60);
        QVERIFY(near(now.value("width").toInt(), 220));
        QVERIFY(near(now.value("height").toInt(), 136));
        // One undo puts all four back.
        app.model.undoStack()->undo();
        QCOMPARE(regionOf(app).value("width").toInt(), 160);
        QCOMPARE(regionOf(app).value("height").toInt(), 96);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void draggingInEmptySpaceDrawsANewRegion() {
        App app(load(fixture(m_canvas)));
        openFollowing(app);
        drag(app, pixelAt(app, 300, 320), pixelAt(app, 420, 400));
        QCOMPARE(app.model.undoStack()->count(), 1);
        const QVariantMap now = regionOf(app);
        QVERIFY(near(now.value("x").toInt(), 300));
        QVERIFY(near(now.value("y").toInt(), 320));
        QVERIFY(near(now.value("width").toInt(), 120));
        QVERIFY(near(now.value("height").toInt(), 80));
        // Up and to the left makes the same kind of box, not a flipped one.
        drag(app, pixelAt(app, 200, 440), pixelAt(app, 150, 400));
        QCOMPARE(app.model.undoStack()->count(), 2);
        QVERIFY(near(regionOf(app).value("x").toInt(), 150));
        QVERIFY(near(regionOf(app).value("width").toInt(), 50));
        // A click on empty picture draws nothing.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, pixelAt(app, 460, 60));
        QCOMPARE(app.model.undoStack()->count(), 2);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aRevisionThatArrivesMidDragLeavesTheDragAndThePictureAlone() {
        App app(load(fixture(m_canvas)));
        openFollowing(app);
        const QPoint from = pixelAt(app, 300, 320), to = pixelAt(app, 420, 400);
        drag(app, from, to, false);
        QVERIFY(find(app.window, "canvasViewFrame")->property("dragging").toBool());
        std::thread run = arrivesLater(m_canvas, m_head, "run1", 200);
        QTRY_VERIFY_WITH_TIMEOUT(model(app)->property("choices").toList().size() > 3, 5000); // the strip has it
        run.join();
        QTest::qWait(300);
        QCOMPARE(model(app)->property("revisionB").toString(), m_head); // the picture under the pointer did not change
        QVERIFY(!find(app.window, "canvasViewNewResult"));
        QVERIFY(find(app.window, "canvasViewFrame")->property("dragging").toBool());
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, to);
        QCOMPARE(app.model.undoStack()->count(), 1); // the drag was committed
        QCOMPARE(model(app)->property("revisionB").toString(), m_head);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #234: one thin status line like Photoshop's, and nothing else of internal text under the picture.
    void oneStatusLineSaysZoomSizeAndTheSelection() {
        App app(load(fixture(m_canvas)));
        openFollowing(app);
        QQuickItem *zoom = find(app.window, "canvasViewStatusZoom");
        QVERIFY(zoom);
        const QString text = zoom->property("text").toString();
        QVERIFY2(QRegularExpression(R"(^\d+% · 512 × 512$)").match(text).hasMatch(), qPrintable(text));
        QCOMPARE(find(app.window, "canvasViewStatusSelection")->property("text").toString(), QString("160 × 96 at 100, 60"));
        QTest::keyClick(app.window, Qt::Key_1);
        QTRY_COMPARE(find(app.window, "canvasViewStatusZoom")->property("text").toString(), QString("100% · 512 × 512"));
        // The removed lines: the store diff, the context paragraph, the how-to, the note while it reads the latest version.
        QVERIFY(!find(app.window, "canvasViewSummary"));
        QVERIFY(!find(app.window, "canvasViewRegionInfo"));
        QVERIFY(!find(app.window, "canvasViewRegionHint"));
        QVERIFY(!find(app.window, "canvasViewRegionNote"));
        QVERIFY(!find(app.window, "sizeNotice")); // the size is the status line's now
        // The pro's numbers sit behind Advanced details.
        QVERIFY(!find(app.window, "canvasViewStatusDetails"));
        click(app, "canvasViewAdvancedDetails");
        QTRY_VERIFY(find(app.window, "canvasViewStatusDetails"));
        const QString details = find(app.window, "canvasViewStatusDetails")->property("text").toString();
        QVERIFY2(details.contains("Context "), qPrintable(details));
        QVERIFY2(details.contains("Model ") && details.contains("Reads the latest version"), qPrintable(details));
        click(app, "canvasViewAdvancedDetails");
        QTRY_VERIFY(!find(app.window, "canvasViewStatusDetails"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theSelectionInTheStatusLineFollowsTheDragAndTheContextLabelIsSmall() {
        App app(load(fixture(m_canvas)));
        openFollowing(app);
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        const qreal k = viewer->property("imageScale").toReal();
        const QPoint corner = pixelAt(app, 260, 156);
        drag(app, corner, corner + QPoint(qRound(40 * k), qRound(20 * k)), false);
        const QString live = find(app.window, "canvasViewStatusSelection")->property("text").toString();
        QVERIFY2(live.startsWith("200 × 116") || live.startsWith("199 × ") || live.startsWith("201 × "), qPrintable(live));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, corner + QPoint(qRound(40 * k), qRound(20 * k)));
        QQuickItem *label = find(app.window, "canvasViewContextLabel");
        QVERIFY(label);
        QVERIFY(label->height() < 30);
        QVERIFY(label->property("text").toString().startsWith("Context "));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void escapeDuringADragLeavesTheNodeAsItWas() {
        App app(load(fixture(m_canvas)));
        openFollowing(app);
        const QPoint from = pixelAt(app, 300, 320), to = pixelAt(app, 420, 400);
        drag(app, from, to, false);
        QVERIFY(find(app.window, "canvasViewFrame")->property("dragging").toBool());
        QTest::keyClick(app.window, Qt::Key_Escape);
        QVERIFY(!find(app.window, "canvasViewFrame")->property("dragging").toBool());
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, to);
        QCOMPARE(app.model.undoStack()->count(), 0);
        QCOMPARE(regionOf(app).value("x").toInt(), 100);
        QCOMPARE(regionOf(app).value("width").toInt(), 160);
        // The dialog stayed open: Escape was the drag's.
        QVERIFY(dialog(app)->property("opened").toBool());
        QVERIFY(find(app.window, "canvasViewStatusSelection")->property("text").toString().startsWith("160 × 96 at 100, 60"));
    }

    void zoomAndPanNeverMoveTheRectangle() {
        App app(load(fixture(m_canvas)));
        openFollowing(app);
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        QQuickItem *picture = find(app.window, "canvasViewViewer");
        // The wheel zooms at the pointer, the region stays where the numbers say.
        const QPointF mid(picture->width() / 2, picture->height() / 2);
        QWheelEvent wheel(mid + picture->mapToScene(QPointF(0, 0)), mid, QPoint(), QPoint(0, 240), Qt::NoButton, {}, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(app.window, &wheel);
        QTRY_VERIFY(viewer->property("zoom").toReal() > 1);
        QCOMPARE(app.model.undoStack()->count(), 0);
        // The middle button pans, even starting on the rectangle.
        const qreal before = viewer->property("centerX").toReal();
        const QPoint inside = pixelAt(app, 180, 108);
        drag(app, inside, inside + QPoint(60, 0), true, Qt::MiddleButton);
        QVERIFY(viewer->property("centerX").toReal() < before);
        QCOMPARE(app.model.undoStack()->count(), 0);
        // Space held, a left drag pans, also over the rectangle.
        const qreal beforeSpace = viewer->property("centerX").toReal();
        QTest::keyPress(app.window, Qt::Key_Space);
        const QPoint again = pixelAt(app, 180, 108);
        drag(app, again, again + QPoint(40, 0));
        QTest::keyRelease(app.window, Qt::Key_Space);
        QVERIFY(viewer->property("centerX").toReal() < beforeSpace);
        QCOMPARE(app.model.undoStack()->count(), 0);
        QCOMPARE(regionOf(app).value("x").toInt(), 100);
        QCOMPARE(regionOf(app).value("width").toInt(), 160);
        // Back to the left button alone: it edits again.
        const QPoint place = pixelAt(app, 100, 60);
        drag(app, place + QPoint(20, 20), place + QPoint(40, 20));
        QCOMPARE(app.model.undoStack()->count(), 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aCandidateShownIsDrawnOnWithoutRepinningTheNode() {
        App app(load(fixture(m_canvas)));
        openFollowing(app);
        showRevision(app, m_a);
        QTRY_COMPARE(model(app)->property("revisionB").toString(), m_a);
        QTRY_VERIFY(find(app.window, "canvasViewFrame"));
        QTest::keyClick(app.window, Qt::Key_0);
        QTest::qWait(50);
        drag(app, pixelAt(app, 300, 320), pixelAt(app, 420, 400));
        QCOMPARE(app.model.undoStack()->count(), 1);
        QCOMPARE(regionOf(app).value("revision").toString(), QString("head")); // the choice of revision is its own button
        QVERIFY(near(regionOf(app).value("width").toInt(), 120));
    }

    void snapPutsTheEdgesOnTheNodesMultipleLive() {
        App app(load(fixture(m_canvas)));
        openFollowing(app);
        QCOMPARE(model(app)->property("region").toMap().value("multiple").toInt(), 16);
        click(app, "canvasViewSnap");
        drag(app, pixelAt(app, 301, 322), pixelAt(app, 421, 398));
        QCOMPARE(app.model.undoStack()->count(), 1);
        const QVariantMap now = regionOf(app);
        for (const char *name : {"x", "y", "width", "height"})
            QCOMPARE(now.value(name).toInt() % 16, 0);
    }

    void linkedNumbersKeepTheRegionReadOnlyWithANote() {
        App app(load(linkedFixture(m_canvas)));
        QVERIFY(app.open());
        app.model.select({"region"});
        openDialog(app);
        QTRY_VERIFY(find(app.window, "canvasViewFrame"));
        QTest::keyClick(app.window, Qt::Key_0);
        QTest::qWait(50);
        QVERIFY(find(app.window, "canvasViewRegionHint")->property("text").toString().contains("linked"));
        QVERIFY(!find(app.window, "canvasViewFrame")->property("editable").toBool());
        drag(app, pixelAt(app, 300, 320), pixelAt(app, 420, 400));
        QCOMPARE(app.model.undoStack()->count(), 0);
        QVERIFY(find(app.window, "canvasViewSnap")->isVisible() && !find(app.window, "canvasViewSnap")->isEnabled());
    }

    // ---- a selection without a hunt (#228, the owner's report: a drag on the picture did nothing without a selected
    // Canvas Region node) ----

    // The only Region node of this canvas is followed with nothing selected, and still while another node is selected.
    void theOnlyRegionNodeIsFollowedWithoutASelection() {
        App app(load(fixture(m_canvas)));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_COMPARE(model(app)->property("followNode").toString(), QString("region"));
        QTRY_VERIFY(find(app.window, "canvasViewFrame"));
        QTest::keyClick(app.window, Qt::Key_0);
        QTest::qWait(50);
        drag(app, pixelAt(app, 300, 320), pixelAt(app, 420, 400));
        QCOMPARE(app.model.undoStack()->count(), 1);
        QVERIFY(near(regionOf(app).value("width").toInt(), 120));
        app.model.select({"text"});
        QTest::qWait(50);
        QCOMPARE(model(app)->property("followNode").toString(), QString("region"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Several Region nodes on this canvas and none selected: a drag draws, then asks which node (by title) it is for; the
    // choice is written as one undo step and remembered.
    void withSeveralRegionNodesADrawnRectangleAsksWhichNode() {
        App app(load(QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "r1": {"type": "fugue/canvas-region", "version": "1.0.0", "inputs": {"canvas": "%1", "x": 0, "y": 0, "width": 64, "height": 64}},
            "r2": {"type": "fugue/canvas-region", "version": "1.0.0", "inputs": {"canvas": "%1", "x": 0, "y": 0, "width": 64, "height": 64}}}},
          "layout": {"nodes": {"r1": {"pos": [20, 20]}, "r2": {"pos": [20, 400]}}}})").arg(m_canvas).toUtf8()));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(find(app.window, "canvasViewFrame")); // the free selection's
        QCOMPARE(model(app)->property("followNode").toString(), QString());
        QVERIFY(!find(app.window, "canvasViewRegionHint")); // nothing drawn yet: the tool strip teaches the drag (#234)
        QTest::keyClick(app.window, Qt::Key_0);
        QTest::qWait(50);
        drag(app, pixelAt(app, 300, 320), pixelAt(app, 420, 400));
        QCOMPARE(app.model.undoStack()->count(), 0); // nothing written yet
        QObject *menu = nullptr;
        QTRY_VERIFY((menu = app.window->findChild<QObject *>("canvasViewRegionChooser")) && menu->property("opened").toBool());
        QStringList texts;
        for (const QVariant &entry : menu->property("entries").toList())
            texts << entry.toMap().value("text").toString();
        QCOMPARE(texts, (QStringList{"Which Canvas Region is it for?", "Canvas Region (r1)", "Canvas Region (r2)"})); // titles
        QVERIFY(QMetaObject::invokeMethod(menu, "choose", Q_ARG(QVariant, 2)));
        QTRY_COMPARE(model(app)->property("followNode").toString(), QString("r2"));
        QTRY_COMPARE(app.model.undoStack()->count(), 1);
        const QVariantMap r2 = app.model.widgets()->regionState("r2");
        QVERIFY(near(r2.value("x").toInt(), 300) && near(r2.value("width").toInt(), 120));
        QCOMPARE(app.model.widgets()->regionState("r1").value("width").toInt(), 64);
        // Remembered: the next drag edits r2 at once.
        drag(app, pixelAt(app, 100, 100), pixelAt(app, 160, 140));
        QCOMPARE(app.model.undoStack()->count(), 2);
        QVERIFY(near(app.model.widgets()->regionState("r2").value("width").toInt(), 60, 6));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // No Region node: a drag draws the rectangle (view-only) and offers to add the node, as one undo step.
    void withNoRegionNodeADrawnRectangleOffersToAddOne() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(find(app.window, "canvasViewFrame"));
        QVERIFY(!find(app.window, "canvasViewRegionHint")); // how to draw is taught by the tool strip's tooltips (#234)
        QVERIFY(!find(app.window, "canvasViewAddRegion"));
        QTest::keyClick(app.window, Qt::Key_0);
        QTest::qWait(50);
        drag(app, pixelAt(app, 300, 320), pixelAt(app, 420, 400));
        QTRY_VERIFY(find(app.window, "canvasViewAddRegion"));
        QVERIFY(find(app.window, "canvasViewRegionHint")->property("text").toString().contains("not saved"));
        QCOMPARE(app.model.undoStack()->count(), 0);
        click(app, "canvasViewAddRegion");
        QTRY_COMPARE(app.model.undoStack()->count(), 1); // the node, its canvas and its rectangle: one step
        QString added;
        for (auto it = app.model.document().graph.nodes.cbegin(); it != app.model.document().graph.nodes.cend(); ++it)
            if (it.value().type == QLatin1String("fugue/canvas-region"))
                added = it.key();
        QVERIFY(!added.isEmpty());
        const QVariantMap state = app.model.widgets()->regionState(added);
        QCOMPARE(state.value("canvas").toString(), m_canvas);
        QVERIFY(near(state.value("x").toInt(), 300) && near(state.value("width").toInt(), 120) && near(state.value("height").toInt(), 80));
        QTRY_COMPARE(model(app)->property("followNode").toString(), added);
        app.model.undoStack()->undo();
        QVERIFY(!app.model.document().graph.nodes.contains(added));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // ---- the node's canvas and the canvas shown (#228, the owner's second report: "This region reads another canvas" in
    // red for a Region node with no canvas yet, and no way out) ----

    QByteArray regionReading(const QString &canvas) const {
        return QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {"region": {"type": "fugue/canvas-region", "version": "1.0.0",
            "inputs": {"canvas": "%1", "x": 10, "y": 10, "width": 64, "height": 64}}}},
          "layout": {"nodes": {"region": {"pos": [20, 20]}}}})").arg(canvas).toUtf8();
    }

    // #246: a Region node with no canvas reads the Active canvas, so drawing a rectangle writes the rectangle and leaves
    // the canvas empty; the view reports the canvas it shows as the active one.
    void aRegionNodeWithNoCanvasStaysOnTheActiveCanvasWhenDrawn() {
        App app(load(regionReading("")));
        QVERIFY(app.open());
        app.model.select({"region"});
        openDialog(app);
        QTRY_COMPARE(model(app)->property("followNode").toString(), QString("region"));
        QTRY_VERIFY(model(app)->property("region").toMap().value("valid").toBool());
        QVERIFY(!find(app.window, "canvasViewCanvasMismatch"));
        QTRY_COMPARE(app.activeCanvas.id(), m_canvas); // the view reports the canvas it shows
        QTest::keyClick(app.window, Qt::Key_0);
        QTest::qWait(50);
        drag(app, pixelAt(app, 300, 320), pixelAt(app, 420, 400));
        QCOMPARE(app.model.undoStack()->count(), 1);
        QCOMPARE(regionOf(app).value("canvas").toString(), QString()); // still the Active canvas
        QVERIFY(near(regionOf(app).value("width").toInt(), 120));
        app.model.undoStack()->undo();
        QCOMPARE(regionOf(app).value("canvas").toString(), QString());
        QCOMPARE(regionOf(app).value("width").toInt(), 64);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Another canvas picked in the view while the node reads its own: plain words and both answers, never a red dead end.
    void aNodeOnAnotherCanvasOffersBothAnswers() {
        PaintDocument other(128, 96);
        other.addLayer(LayerKind::Paint, "Base");
        const QString otherId = canvas::CanvasStore::forSession().create("Poster", other, "new")->id;
        App app(load(regionReading(m_canvas)));
        QVERIFY(app.open());
        app.model.select({"region"});
        openDialog(app);
        QTRY_COMPARE(view(app)->property("canvasId").toString(), m_canvas); // the node's canvas, selected
        view(app)->setProperty("canvasId", otherId); // the person picks another one
        QTRY_VERIFY(find(app.window, "canvasViewCanvasMismatch"));
        QCOMPARE(find(app.window, "canvasViewCanvasMismatchText")->property("text").toString(), QString("This Canvas Region reads Sketch, not the canvas shown."));
        QVERIFY(!find(app.window, "canvasViewRegionRow")); // no red line
        QCOMPARE(find(app.window, "canvasViewUseCanvasForNode")->property("text").toString(), QString("Use Poster for this node"));
        click(app, "canvasViewShowNodeCanvas");
        QTRY_COMPARE(view(app)->property("canvasId").toString(), m_canvas);
        QTRY_VERIFY(!find(app.window, "canvasViewCanvasMismatch"));
        view(app)->setProperty("canvasId", otherId);
        click(app, "canvasViewUseCanvasForNode");
        QTRY_COMPARE(regionOf(app).value("canvas").toString(), otherId);
        QCOMPARE(app.model.undoStack()->count(), 1);
        QTRY_VERIFY(!find(app.window, "canvasViewCanvasMismatch"));
        QTRY_VERIFY(model(app)->property("region").toMap().value("valid").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // A pinned node offers the way back: one click and it follows the Active canvas again (#246).
    void aPinnedNodeCanGoBackToTheActiveCanvas() {
        PaintDocument other(128, 96);
        other.addLayer(LayerKind::Paint, "Base");
        const QString otherId = canvas::CanvasStore::forSession().create("Poster", other, "new")->id;
        App app(load(regionReading(m_canvas)));
        QVERIFY(app.open());
        app.model.select({"region"});
        openDialog(app);
        QTRY_COMPARE(view(app)->property("canvasId").toString(), m_canvas);
        QVERIFY(model(app)->property("region").toMap().value("pinned").toBool());
        view(app)->setProperty("canvasId", otherId);
        QTRY_COMPARE(app.activeCanvas.id(), otherId); // choosing a canvas in the view makes it the active one
        QTRY_VERIFY(find(app.window, "canvasViewCanvasMismatch"));
        QQuickItem *button = find(app.window, "canvasViewUseActiveForNode");
        QVERIFY(button && button->isVisible());
        QCOMPARE(button->property("text").toString(), QString("Use the active canvas"));
        // The banner's row sits past the test window's foot, so the tap is sent to the button, not the pointer.
        QVERIFY(QMetaObject::invokeMethod(button, "tapped"));
        QTRY_COMPARE(regionOf(app).value("canvas").toString(), QString());
        QCOMPARE(app.model.undoStack()->count(), 1);
        QTRY_VERIFY(!find(app.window, "canvasViewCanvasMismatch")); // no notice: nothing is pinned any more
        QVERIFY(model(app)->property("region").toMap().value("active").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // An id this root has none of: said so, and the shown canvas can be used instead (there is nothing to show).
    void aNodeOnAnUnknownCanvasSaysSoAndCanUseTheShownOne() {
        App app(load(regionReading("canvas-0123456789ab")));
        QVERIFY(app.open());
        app.model.select({"region"});
        openDialog(app);
        QTRY_VERIFY(find(app.window, "canvasViewCanvasMismatch"));
        QVERIFY(find(app.window, "canvasViewCanvasMismatchText")->property("text").toString().contains("this root does not have"));
        QVERIFY(!find(app.window, "canvasViewShowNodeCanvas"));
        click(app, "canvasViewUseCanvasForNode");
        QTRY_COMPARE(regionOf(app).value("canvas").toString(), view(app)->property("canvasId").toString());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The History panel (#228, the owner's decision: no strip, no hashes; Photoshop's History): steps in words, newest
    // first; a click shows that step read only and Back to latest returns; All revisions adds the candidates.
    void theHistoryPanelListsStepsInWordsAndShowsOne() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(!model(app)->property("revisionB").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(model(app), "setOpacity", Q_ARG(QVariantList, QVariantList{0}), Q_ARG(double, 0.5)));
        QTRY_VERIFY(model(app)->property("head").toString() != m_head);
        viaMenu(app, "View", "canvas-view.history");
        QTRY_VERIFY(find(app.window, "canvasHistoryPanel"));
        QQuickItem *list = find(app.window, "canvasHistoryList");
        QTRY_COMPARE(list->property("count").toInt(), 2);
        QCOMPARE(model(app)->property("steps").toList().at(0).toMap().value("name").toString(), QString("Opacity 50%: Base"));
        QCOMPARE(model(app)->property("steps").toList().at(1).toMap().value("name").toString(), QString("Canvas made"));
        QVERIFY(!find(app.window, "canvasHistoryBackLatest"));
        click(app, "canvasHistoryStep-1");
        QTRY_COMPARE(model(app)->property("revisionB").toString(), m_head);
        QTRY_VERIFY(find(app.window, "canvasLayersReadOnly")); // an earlier step: read only, and says so
        click(app, "canvasHistoryBackLatest");
        QTRY_COMPARE(model(app)->property("revisionB").toString(), model(app)->property("head").toString());
        click(app, "canvasHistoryShowAll");
        QTRY_COMPARE(list->property("count").toInt(), 4); // the two candidates, first
        // The picture's label is a word, never a hash.
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        QCOMPARE(viewer->property("labelA").toString(), QString("Latest"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // ---- the view's own menu bar (#232 slice B) ----

    void theMenuBarHasFiveMenusAndHonestPlaceholders() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        for (const char *title : {"Canvas", "Edit", "Select", "Layer", "View"})
            QTRY_VERIFY2(find(app.window, QString("canvasMenuTitle-") + title), title);
        // A planned feature is a greyed row that is no command: it has no key and pressing it does nothing.
        QQuickItem *placeholder = menuRow(app, "Canvas", "canvas-view.new"); // opens File
        QVERIFY(placeholder);
        QQuickItem *planned = find(app.window, "menuPlanned-236-Place picture…");
        QVERIFY(planned);
        QVERIFY(!planned->isEnabled());
        QCOMPARE(planned->property("plannedNote").toString(), QString("Not built yet (#236)"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(planned));
        QVERIFY(dialog(app)->property("opened").toBool());
        closeMenu(app);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void deselectClearsTheRegionsRectangleInOneUndoStep() {
        App app(load(fixture(m_canvas)));
        openFollowing(app);
        QCOMPARE(regionOf(app).value("width").toInt(), 160);
        QCOMPARE(app.model.undoStack()->count(), 0);
        viaMenu(app, "Select", "canvas-view.deselect");
        QTRY_COMPARE(regionOf(app).value("width").toInt(), 0);
        QCOMPARE(regionOf(app).value("height").toInt(), 0);
        QCOMPARE(app.model.undoStack()->count(), 1);
        QQuickItem *row = menuRow(app, "Select", "canvas-view.deselect");
        QVERIFY(row && !row->isEnabled()); // nothing is selected now
        closeMenu(app);
        app.model.undoStack()->undo();
        QTRY_COMPARE(regionOf(app).value("width").toInt(), 160);
    }

    void escapeAndCtrlDClearTheSelectionAndTheDialogStaysOpen() {
        App app(load(fixture(m_canvas)));
        openFollowing(app);
        QTest::mouseMove(app.window, pixelAt(app, 50, 50)); // the pointer is over the view: its keys are live
        QTest::keyClick(app.window, Qt::Key_D, Qt::ControlModifier);
        QTRY_COMPARE(regionOf(app).value("width").toInt(), 0);
        app.model.undoStack()->undo();
        QTRY_COMPARE(regionOf(app).value("width").toInt(), 160);
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_COMPARE(regionOf(app).value("width").toInt(), 0);
        QVERIFY(dialog(app)->property("opened").toBool()); // the first Escape was the selection's
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog(app)->property("opened").toBool()); // nothing left to clear: it closes
    }

    void aFreeRectangleIsDeselectedAndSelectAllDrawsTheWholeCanvas() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(find(app.window, "canvasViewFrame") || view(app)->property("freeMode").toBool());
        QVERIFY(!view(app)->property("hasFreeRect").toBool());
        viaMenu(app, "Select", "canvas-view.select-all");
        QTRY_VERIFY(view(app)->property("hasFreeRect").toBool());
        QCOMPARE(view(app)->property("freeRect").toMap().value("width").toInt(), 512);
        viaMenu(app, "Select", "canvas-view.deselect");
        QTRY_VERIFY(!view(app)->property("hasFreeRect").toBool());
    }

    void aPaintedSelectionIsDeselectedAndInverseIsForIt() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(find(app.window, "canvasViewViewer"));
        QQuickItem *inverse = menuRow(app, "Select", "canvas-view.invert-selection");
        QVERIFY(inverse && !inverse->isEnabled()); // there is no painted selection yet
        closeMenu(app);
        viaMenu(app, "Select", "canvas-view.quick-mask");
        QTRY_VERIFY(view(app)->property("quickMask").toBool());
        QObject *mask = view(app)->property("selectionMask").value<QObject *>();
        QVERIFY(QMetaObject::invokeMethod(mask, "fillRect", Q_ARG(double, 10), Q_ARG(double, 10), Q_ARG(double, 40), Q_ARG(double, 40), Q_ARG(bool, false)));
        QTRY_VERIFY(view(app)->property("maskSelected").toBool());
        viaMenu(app, "Select", "canvas-view.invert-selection");
        QTRY_VERIFY(view(app)->property("maskSelected").toBool());
        viaMenu(app, "Select", "canvas-view.deselect");
        QTRY_VERIFY(!view(app)->property("maskSelected").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #251: Select > Modify and Transform selection act on the painted selection, or on a rectangle painted into it first;
    // each is one step of the selection's undo.
    void modifyAndTransformSelectionAreCommandsWithSmallDialogs() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(find(app.window, "canvasViewViewer"));
        const auto row = [&](const QString &command, bool modify) {
            menuRow(app, "Select", "canvas-view.select-all");
            if (modify)
                click(app, "subMenu-Modify");
            QQuickItem *item = nullptr;
            for (int i = 0; i < 100 && !item; ++i) {
                item = find(app.window, "menuItem-" + command);
                if (!item)
                    QTest::qWait(20);
            }
            return item;
        };
        // Nothing selected: every row is greyed, and none is a placeholder any more.
        for (const char *command : {"expand", "contract", "feather", "border", "smooth"}) {
            QQuickItem *item = row(QString("canvas-view.selection-") + command, true);
            QVERIFY(item && !item->isEnabled());
            closeMenu(app);
        }
        QQuickItem *transform = row("canvas-view.selection-transform", false);
        QVERIFY(transform && !transform->isEnabled());
        QVERIFY(!find(app.window, "menuPlanned-251-Modify selection…"));
        closeMenu(app);
        // A rectangle: Contract paints it into the selection and takes 3 px off every side.
        const QVariant rect = QVariantMap{{"x", 20}, {"y", 30}, {"width", 100}, {"height", 80}};
        QVERIFY(QMetaObject::invokeMethod(view(app), "placeFree", Q_ARG(QVariant, rect)));
        QTRY_VERIFY(view(app)->property("hasFreeRect").toBool());
        QQuickItem *contract = row("canvas-view.selection-contract", true);
        QVERIFY(contract && contract->isEnabled());
        click(app, "menuItem-canvas-view.selection-contract");
        QTRY_VERIFY(find(app.window, "canvasModifyAmount"));
        find(app.window, "canvasModifyAmount")->setProperty("text", "3");
        click(app, "canvasModifyOk");
        QTRY_VERIFY(view(app)->property("maskSelected").toBool());
        QVERIFY(!view(app)->property("hasFreeRect").toBool());
        QVERIFY(view(app)->property("quickMask").toBool()); // the painted selection is shown as the Quick Mask
        QObject *mask = view(app)->property("selectionMask").value<QObject *>();
        const auto valueAt = [&](int x, int y) {
            int v = -1;
            QMetaObject::invokeMethod(mask, "valueAt", Q_RETURN_ARG(int, v), Q_ARG(int, x), Q_ARG(int, y));
            return v;
        };
        QCOMPARE(valueAt(22, 60), 0);
        QCOMPARE(valueAt(23, 60), 255);
        QCOMPARE(valueAt(60, 32), 0);
        QCOMPARE(valueAt(60, 33), 255);
        // One step: Undo gives the rectangle as painted.
        viaMenu(app, "Edit", "canvas-view.undo");
        QTRY_COMPARE(valueAt(20, 60), 255);
        // Expand it again on the painted selection, with the dialog's Enter.
        QVERIFY(QMetaObject::invokeMethod(mask, "clear"));
        QVERIFY(QMetaObject::invokeMethod(mask, "fillRect", Q_ARG(double, 100), Q_ARG(double, 100), Q_ARG(double, 40), Q_ARG(double, 40), Q_ARG(bool, false)));
        QTRY_VERIFY(view(app)->property("maskSelected").toBool());
        row("canvas-view.selection-expand", true);
        click(app, "menuItem-canvas-view.selection-expand");
        QTRY_VERIFY(find(app.window, "canvasModifyAmount"));
        find(app.window, "canvasModifyAmount")->setProperty("text", "5");
        click(app, "canvasModifyOk");
        QTRY_COMPARE(valueAt(95, 120), 255);
        QCOMPARE(valueAt(94, 120), 0);
        // Transform: move 10 right and 20 down.
        viaMenu(app, "Select", "canvas-view.selection-transform");
        QTRY_VERIFY(find(app.window, "canvasTransformMoveX"));
        find(app.window, "canvasTransformMoveX")->setProperty("text", "10");
        find(app.window, "canvasTransformMoveY")->setProperty("text", "20");
        click(app, "canvasTransformOk");
        QTRY_COMPARE(valueAt(105, 140), 255);
        QCOMPARE(valueAt(104, 140), 0);
        QCOMPARE(valueAt(105, 114), 0);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void renameDuplicateClearDeleteAndCloseAreInTheFileMenu() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_COMPARE(model(app)->property("canvasId").toString(), m_canvas);
        // Rename asks for the name.
        viaMenu(app, "Canvas", "canvas-view.rename");
        QTRY_VERIFY(find(app.window, "canvasRenameField"));
        find(app.window, "canvasRenameField")->setProperty("text", "Poster");
        click(app, "canvasRenameOk");
        QTRY_COMPARE(canvas::CanvasStore::forSession().info(m_canvas)->name, QString("Poster"));
        // Clear asks first (Keep leaves it), then is one step that Undo takes back.
        viaMenu(app, "Canvas", "canvas-view.clear");
        click(app, "canvasClearKeep");
        QCOMPARE(model(app)->property("revisionB").toString(), m_head);
        viaMenu(app, "Canvas", "canvas-view.clear");
        click(app, "canvasClearYes");
        QTRY_VERIFY(model(app)->property("revisionB").toString() != m_head);
        QCOMPARE(model(app)->property("layers").toList().size(), 1);
        QVERIFY(QMetaObject::invokeMethod(model(app), "undo"));
        QTRY_COMPARE(model(app)->property("revisionB").toString(), m_head);
        // Duplicate shows the copy; Delete asks, removes it and shows the other.
        viaMenu(app, "Canvas", "canvas-view.duplicate");
        QTRY_VERIFY(model(app)->property("canvasId").toString() != m_canvas);
        const QString copy = model(app)->property("canvasId").toString();
        QCOMPARE(canvas::CanvasStore::forSession().info(copy)->name, QString("Poster copy"));
        viaMenu(app, "Canvas", "canvas-view.delete");
        click(app, "canvasDeleteKeep");
        QVERIFY(canvas::CanvasStore::forSession().info(copy).ok());
        viaMenu(app, "Canvas", "canvas-view.delete");
        click(app, "canvasDeleteYes");
        QTRY_COMPARE(model(app)->property("canvasId").toString(), m_canvas);
        QVERIFY(!canvas::CanvasStore::forSession().info(copy).ok());
        // Close closes the dialog that hosts the view.
        viaMenu(app, "Canvas", "canvas-view.close");
        QTRY_VERIFY(!dialog(app)->property("opened").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #249: Share… asks what goes in, and the model's package opens again as a canvas of its own, history and all, with a
    // notice; the same file again is a second canvas, never the first one replaced.
    void shareAsksThenAPackageImportsAsANewCanvas() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_COMPARE(model(app)->property("canvasId").toString(), m_canvas);
        viaMenu(app, "Canvas", "canvas-view.share");
        QTRY_VERIFY(find(app.window, "canvasShareHead"));
        QVERIFY(find(app.window, "canvasShareWithHistory"));
        const QString file = m_dir.filePath("shared.fuguepack");
        bool written = false;
        QVERIFY(QMetaObject::invokeMethod(model(app), "sharePackage", Q_RETURN_ARG(bool, written), Q_ARG(QString, file), Q_ARG(bool, true)));
        QVERIFY(written);
        QVERIFY(QFileInfo(file).isFile());
        QString problem = "unset";
        QVERIFY(QMetaObject::invokeMethod(model(app), "importPicture", Q_RETURN_ARG(QString, problem), Q_ARG(QString, file), Q_ARG(QString, QString())));
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QTRY_VERIFY(model(app)->property("canvasId").toString() != m_canvas);
        QVERIFY(model(app)->property("notice").toString().contains("history"));
        QVERIFY(model(app)->property("notice").toString().contains("untouched")); // its id was taken here
        QVERIFY(canvas::CanvasStore::forSession().info(m_canvas).ok());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #252: the Layer menu's rows are live, and the properties dialog is one revision.
    void theLayerMenuMakesMergesGroupsAndOpensTheProperties() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_COMPARE(model(app)->property("canvasId").toString(), m_canvas);
        QTRY_COMPARE(model(app)->property("layers").toList().size(), 1);
        // One layer: Merge down and Ungroup have nothing to act on, New layer and Flatten do.
        QQuickItem *merge = menuRow(app, "Layer", "canvas-view.merge-down");
        QVERIFY(merge && !merge->isEnabled());
        closeMenu(app);
        QQuickItem *ungroup = menuRow(app, "Layer", "canvas-view.ungroup");
        QVERIFY(ungroup && !ungroup->isEnabled());
        closeMenu(app);
        viaMenu(app, "Layer", "canvas-view.new-layer");
        QTRY_COMPARE(model(app)->property("layers").toList().size(), 2);
        QCOMPARE(model(app)->property("layers").toList().first().toMap().value("name").toString(), QString("Layer 2"));
        // Two shown paint layers: Merge down is on, and one revision takes them to one.
        QQuickItem *mergeNow = menuRow(app, "Layer", "canvas-view.merge-down");
        QVERIFY(mergeNow && mergeNow->isEnabled());
        closeMenu(app);
        viaMenu(app, "Layer", "canvas-view.merge-down");
        QTRY_COMPARE(model(app)->property("layers").toList().size(), 1);
        viaMenu(app, "Layer", "canvas-view.new-layer");
        QTRY_COMPARE(model(app)->property("layers").toList().size(), 2);
        // Group: the selected layer (the new one) goes into a group.
        viaMenu(app, "Layer", "canvas-view.group");
        QTRY_COMPARE(model(app)->property("layers").toList().first().toMap().value("kind").toString(), QString("group"));
        viaMenu(app, "Layer", "canvas-view.ungroup");
        QTRY_COMPARE(model(app)->property("layers").toList().first().toMap().value("kind").toString(), QString("paint"));
        // Layer properties…: Name and Opacity in one dialog, one revision, one undo.
        const int revisions = int(canvas::CanvasStore::forSession().log(m_canvas)->size());
        viaMenu(app, "Layer", "canvas-view.layer-properties");
        QTRY_VERIFY(find(app.window, "canvasPropertiesName"));
        find(app.window, "canvasPropertiesName")->setProperty("text", "Sky");
        click(app, "canvasPropertiesBlend-multiply");
        click(app, "canvasPropertiesOk");
        QTRY_COMPARE(model(app)->property("layers").toList().first().toMap().value("name").toString(), QString("Sky"));
        QCOMPARE(model(app)->property("layers").toList().first().toMap().value("blend").toString(), QString("multiply"));
        QCOMPARE(int(canvas::CanvasStore::forSession().log(m_canvas)->size()), revisions + 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // ---- the cohesion batch (#228, .ui-ergonomics/2026-11-22-canvas-areas-cohesion.md) ----

    // L2: one bar. The frame's own bar is off; Fit, 100% and the close end the view's tools row, in that order.
    void theDialogHasOneBarEndingInFitActualSizeAndClose() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(find(app.window, "canvasViewClose"));
        QVERIFY(!find(app.window, "canvasViewZoom-fit")); // the frame's own
        QQuickItem *fit = find(app.window, "canvasViewFit"), *actual = find(app.window, "canvasViewActual"),
                   *close = find(app.window, "canvasViewClose");
        QVERIFY(fit && actual && close);
        QVERIFY(!find(app.window, "canvasViewLayers")); // the View menu has Layers and History now
        const auto x = [](QQuickItem *item) { return item->mapToScene(QPointF(0, 0)).x(); };
        const auto y = [](QQuickItem *item) { return item->mapToScene(QPointF(0, 0)).y(); };
        QTRY_VERIFY(x(fit) < x(actual) && x(actual) < x(close)); // once laid out
        QCOMPARE(y(close), y(fit)); // one row
        click(app, "canvasViewClose");
        QTRY_VERIFY(!dialog(app)->property("opened").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // D5: Home fits as in the Graph; Enter does nothing here (#239: no candidate to accept; it is the host's).
    void homeFitsAndEnterDoesNothingInTheView() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(find(app.window, "canvasViewViewer")));
        QTest::keyClick(app.window, Qt::Key_1);
        QTRY_VERIFY(viewer->property("actual").toBool());
        QTest::keyClick(app.window, Qt::Key_Home);
        QTRY_VERIFY(!viewer->property("actual").toBool());
        QTest::keyClick(app.window, Qt::Key_Return);
        QCOMPARE(model(app)->property("head").toString(), m_head);
        QVERIFY(dialog(app)->property("opened").toBool());
        showRevision(app, m_a);
        QTRY_COMPARE(model(app)->property("revisionB").toString(), m_a);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(find(app.window, "canvasViewViewer")));
        QTest::keyClick(app.window, Qt::Key_Return);
        QCOMPARE(model(app)->property("head").toString(), m_head); // a revision kept apart is not accepted by Enter
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // L13 and D7: the five compare modes of Compare Images, the mode remembered; toggles and the strip in the quiet marking.
    void theCompareModesAreCompareImagesFiveAndTogglesAreQuiet() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        showRevision(app, m_a);
        QTRY_COMPARE(model(app)->property("revisionB").toString(), m_a);
        viaMenu(app, "View", "canvas-view.compare");
        for (const char *mode : {"slider", "slider_horizontal", "side_by_side", "flip", "difference"})
            QTRY_VERIFY2(find(app.window, QStringLiteral("canvasViewMode-") + mode), mode);
        click(app, "canvasViewMode-slider_horizontal");
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        QTRY_COMPARE(viewer->property("mode").toString(), QString("slider_horizontal"));
        // Compare on and its mode are quiet.
        for (const char *name : {"canvasViewMode-slider_horizontal"})
            QVERIFY2(!find(app.window, name)->property("marked").toBool() && find(app.window, name)->property("current").toBool(), name);
        QQuickItem *compareRow = menuRow(app, "View", "canvas-view.compare");
        QVERIFY(compareRow && compareRow->property("checkable").toBool() && compareRow->property("checked").toBool()); // on, in the menu
        closeMenu(app);
        viaMenu(app, "View", "canvas-view.compare");
        QTRY_COMPARE(viewer->property("mode").toString(), QString("single"));
        viaMenu(app, "View", "canvas-view.compare"); // the mode last picked comes back
        QTRY_COMPARE(viewer->property("mode").toString(), QString("slider_horizontal"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // L5: the view's canvas menu has the picker's rows: size and last change, the id, New canvas… and Import picture….
    void theCanvasMenuHasThePickersRowsAndMakesACanvas() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        click(app, "canvasViewCanvas");
        QObject *menu = nullptr;
        QTRY_VERIFY((menu = app.window->findChild<QObject *>("widgetMenu")) && menu->property("opened").toBool());
        const QVariantList entries = menu->property("entries").toList();
        QStringList texts;
        for (const QVariant &entry : entries)
            texts << entry.toMap().value("text").toString();
        QVERIFY2(texts.contains("Sketch") && texts.contains("New canvas…") && texts.contains("Import picture…"), qPrintable(texts.join(", ")));
        const QVariantMap sketch = entries.at(int(texts.indexOf("Sketch"))).toMap();
        QVERIFY(sketch.value("detail").toString().startsWith("512 × 512 · "));
        QCOMPARE(sketch.value("note").toString(), m_canvas);
        QVERIFY(QMetaObject::invokeMethod(menu, "picked", Q_ARG(QVariant, entries.at(int(texts.indexOf("New canvas…"))).toMap().value("value"))));
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QObject *ask = nullptr;
        QTRY_VERIFY((ask = app.window->findChild<QObject *>("newCanvasDialog")) && ask->property("opened").toBool());
        ask->property("model").value<QObject *>()->setProperty("name", "Poster");
        QVERIFY(QMetaObject::invokeMethod(ask, "create"));
        QTRY_VERIFY(!ask->property("opened").toBool());
        QTRY_COMPARE(view(app)->property("canvasId").toString() != m_canvas, true);
        QCOMPARE(canvas::CanvasStore::forSession().info(view(app)->property("canvasId").toString())->name, QString("Poster"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // L18: right to left the bar mirrors (the close at its far end, the left) and the New canvas question's buttons with
    // it; the picture does not.
    void rightToLeftMirrorsTheBarAndTheQuestionButNotThePicture() {
        struct Back { ~Back() { QGuiApplication::setLayoutDirection(Qt::LeftToRight); } } back;
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        QGuiApplication::setLayoutDirection(Qt::RightToLeft); // after open: the window's translator sets it from the language
        openDialog(app);
        QTRY_VERIFY(find(app.window, "canvasViewClose"));
        const auto x = [](QQuickItem *item) { return item->mapToScene(QPointF(0, 0)).x(); };
        QTRY_VERIFY(x(find(app.window, "canvasViewClose")) < x(find(app.window, "canvasViewFit"))); // once laid out
        QTRY_VERIFY(x(find(app.window, "canvasMenuTitle-Canvas")) > x(find(app.window, "canvasMenuTitle-View"))); // File at the reading end
        QQuickItem *picture = find(app.window, "canvasViewViewer");
        QQuickItem *layers = find(app.window, "canvasLayersPanel");
        QVERIFY(layers && x(layers) < x(picture)); // the panel at the reading end, the left
        // The picture is drawn as it is: its first label stays at its left edge.
        auto *label = picture->findChild<QQuickItem *>("labelA");
        QVERIFY(label && label->x() < picture->width() / 2);
        QVERIFY(QMetaObject::invokeMethod(view(app), "newCanvas"));
        QObject *ask = nullptr;
        QTRY_VERIFY((ask = app.window->findChild<QObject *>("newCanvasDialog")) && ask->property("opened").toBool());
        QTest::qWait(50);
        auto *ok = ask->findChild<QQuickItem *>("newCanvasOk");
        auto *cancel = ask->findChild<QQuickItem *>("newCanvasCancel");
        QVERIFY(ok && cancel);
        QVERIFY(x(ok) < x(cancel)); // Create at the left end: the primary last in reading order
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Pictures: FUGUE_SHOT_DIR=<dir> test_canvas_view_window savesThePicturesOfTheWindow (nothing without the variable),
    // dark and light: a revision with a region followed, the canvas menu, the
    // New canvas question, and the empty store.
    void savesThePicturesOfTheWindow_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("name");
        QTest::newRow("dark") << "tokyo-night" << "dark";
        QTest::newRow("light") << "catppuccin-latte" << "light";
    }
    void savesThePicturesOfTheWindow() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("FUGUE_SHOT_DIR is not set");
        QFETCH(QString, theme);
        QFETCH(QString, name);
        const auto save = [&](App &app, const QString &what) {
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/canvas-view-%2-%3.png").arg(QString::fromLocal8Bit(dir), name, what)));
        };
        {
            App app(load(fixture(m_canvas)), {}, Look{theme, 8});
            QVERIFY(app.open());
            app.model.select({"region"});
            openDialog(app);
            showRevision(app, m_a);
            QTRY_VERIFY(find(app.window, "canvasViewRegion"));
            save(app, "candidate");
            QTest::keyClick(app.window, Qt::Key_0);
            click(app, "canvasViewAdvancedDetails");
            QTRY_VERIFY(find(app.window, "canvasViewStatusDetails"));
            save(app, "status-line");
            click(app, "canvasViewAdvancedDetails");
            viaMenu(app, "View", "canvas-view.history");
            QTRY_VERIFY(find(app.window, "canvasHistoryPanel"));
            save(app, "history");
            viaMenu(app, "View", "canvas-view.history");
            showRevision(app, model(app)->property("head").toString());
            QTRY_COMPARE(model(app)->property("revisionB").toString(), m_head);
            click(app, "canvasViewCanvas");
            QTRY_VERIFY(app.window->findChild<QObject *>("widgetMenu"));
            save(app, "menu");
        }
        {
            App app(load(fixture(m_canvas)), {}, Look{theme, 8});
            QVERIFY(app.open());
            openDialog(app);
            QVERIFY(QMetaObject::invokeMethod(view(app), "newCanvas"));
            QTRY_VERIFY(app.window->findChild<QObject *>("newCanvasDialog"));
            save(app, "new-canvas");
        }
        QDir(m_dir.filePath("home")).removeRecursively();
        QDir(m_dir.filePath("data")).removeRecursively();
        {
            App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"), {}, Look{theme, 8});
            QVERIFY(app.open());
            QVERIFY(app.commands.trigger("file.canvas-view"));
            QTRY_VERIFY(find(app.window, "canvasViewEmptyNew"));
            save(app, "empty");
        }
    }

    void aStoreWithNoCanvasSaysSo() {
        QDir(m_dir.filePath("home")).removeRecursively();
        QDir(m_dir.filePath("data")).removeRecursively();
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("file.canvas-view"));
        QTRY_VERIFY(dialog(app) && dialog(app)->property("opened").toBool());
        QTRY_VERIFY(find(app.window, "canvasViewEmpty"));
        QCOMPARE(find(app.window, "canvasViewEmptyText")->property("text").toString(), QString("No canvases yet"));
        // What to do in the app, not on the command line (#228): the canvas picker's two ways.
        QVERIFY(find(app.window, "canvasViewEmptyNew"));
        QVERIFY(find(app.window, "canvasViewEmptyImport"));
    }
};

QTEST_MAIN(TestCanvasViewWindow)
#include "test_canvas_view_window.moc"
