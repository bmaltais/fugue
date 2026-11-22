// The Canvas view in the real window (#221, epics #216/#217, spec 14.5), offscreen: File > Canvas… opens the picture of the
// newest canvas at its head; the strip picks a revision, a candidate shows over the head (Show candidate puts the head back),
// Accept and Drop act on it, Compare opens the modes, the wheel and keys zoom, and a Canvas Region selected in the graph is
// the follow target: its rectangle is drawn on the picture and "Region reads from this revision" is one undo step. The store
// is the test's own (FUGUE_HOME and the XDG folders point at a temporary folder); no window is launched outside the test
// process and no real pointer is moved.

#include <QDir>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtTest>

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

} // namespace

class TestCanvasViewWindow : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_canvas, m_head, m_a, m_b;

    static QString shortOf(const QString &id) { return id.mid(7, 8); }

    QObject *dialog(App &app) const { return app.window->findChild<QObject *>("canvasViewDialog"); }
    QObject *view(App &app) const { return dialog(app)->property("view").value<QObject *>(); }
    QObject *model(App &app) const { return view(app)->property("backend").value<QObject *>(); }
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
        QTRY_VERIFY(find(app.window, "canvasViewChoice-head"));
        QVERIFY(find(app.window, "canvasViewChoice-" + shortOf(m_a)));
        QVERIFY(find(app.window, "canvasViewChoice-" + shortOf(m_b)));
        QCOMPARE(model(app)->property("revisionB").toString(), m_head);
        // The head alone: nothing to accept, drop or overlay; Export is there.
        QVERIFY(!find(app.window, "canvasViewAccept"));
        QVERIFY(!find(app.window, "canvasViewOverlay"));
        QVERIFY(find(app.window, "canvasViewExport"));
        QVERIFY(!find(app.window, "canvasViewRegionRow")); // nothing followed
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aCandidateShowsOverTheHeadAndTheToggleBringsTheHeadBack() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        click(app, "canvasViewChoice-" + shortOf(m_a));
        QTRY_COMPARE(model(app)->property("revisionB").toString(), m_a);
        QVERIFY(find(app.window, "canvasViewAccept"));
        QVERIFY(find(app.window, "canvasViewDrop"));
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        QCOMPARE(viewer->property("sourceA").toUrl(), model(app)->property("sourceB").toUrl()); // the candidate
        click(app, "canvasViewOverlay");
        QTRY_COMPARE(viewer->property("sourceA").toUrl(), model(app)->property("headSource").toUrl()); // the head alone
        click(app, "canvasViewOverlay");
        QTRY_COMPARE(viewer->property("sourceA").toUrl(), model(app)->property("sourceB").toUrl());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void compareOpensTheModesAgainstTheHead() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        click(app, "canvasViewChoice-" + shortOf(m_a));
        QTRY_COMPARE(model(app)->property("revisionB").toString(), m_a);
        QVERIFY(!find(app.window, "canvasViewMode-slider"));
        QVERIFY(find(app.window, "canvasViewOverlay")); // the overlay toggle is for the picture alone
        click(app, "canvasViewCompare");
        QTRY_VERIFY(find(app.window, "canvasViewMode-slider"));
        QVERIFY(!find(app.window, "canvasViewOverlay"));
        QObject *viewer = view(app)->property("viewer").value<QObject *>();
        QTRY_COMPARE(viewer->property("mode").toString(), QString("slider"));
        QCOMPARE(viewer->property("sourceA").toUrl(), model(app)->property("sourceA").toUrl());
        QCOMPARE(model(app)->property("revisionA").toString(), m_head);
        click(app, "canvasViewMode-difference");
        QTRY_COMPARE(viewer->property("mode").toString(), QString("difference"));
        click(app, "canvasViewCompare");
        QTRY_COMPARE(viewer->property("mode").toString(), QString("single"));
    }

    void acceptAndDropAct() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        click(app, "canvasViewChoice-" + shortOf(m_b));
        click(app, "canvasViewDrop");
        QTRY_VERIFY(!find(app.window, "canvasViewChoice-" + shortOf(m_b)));
        click(app, "canvasViewChoice-" + shortOf(m_a));
        click(app, "canvasViewAccept");
        QTRY_COMPARE(model(app)->property("head").toString(), m_a);
        QTRY_VERIFY(!find(app.window, "canvasViewAccept"));
        QCOMPARE(model(app)->property("revisionB").toString(), m_a);
        const auto info = canvas::CanvasStore::forSession().info(m_canvas);
        QCOMPARE(info->head, m_a);
    }

    void aStoreChangeWhileOpenShowsWithoutAClick() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        openDialog(app);
        QTRY_VERIFY(find(app.window, "canvasViewChoice-head"));
        PaintDocument base(512, 512);
        base.addLayer(LayerKind::Paint, "Base");
        const QString c = canvas::CanvasStore::forSession()
                              .addRevision(m_canvas, withFill(base, "Fill C", 200), m_head, "canvas-write:c")
                              .value()
                              .id;
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "canvasViewChoice-" + shortOf(c)), 4000);
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
        // Panned by a drag at 100%.
        const qreal before = viewer->property("centerX").toReal();
        const QPoint from = centerOf(picture);
        QTest::mouseMove(app.window, from);
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        QTest::mouseMove(app.window, from + QPoint(40, 0));
        QTest::mouseMove(app.window, from + QPoint(80, 0));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, from + QPoint(80, 0));
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
        const QRectF rectangle = layer->property("rectangle").toRectF();
        const qreal k = viewer->property("imageScale").toReal();
        QVERIFY(k > 0);
        QVERIFY(qAbs(rectangle.width() - 160 * k) < 0.5);
        QVERIFY(qAbs(rectangle.height() - 96 * k) < 0.5);
        QVERIFY(qAbs(rectangle.x() - (viewer->property("imageX").toReal() + 100 * k)) < 0.5);
        const QRectF context = layer->property("context").toRectF();
        QVERIFY(qAbs(context.width() - 200 * k) < 0.5);
        // It reads the head, and the head is shown: nothing to write.
        QQuickItem *use = find(app.window, "canvasViewUseRegion");
        QVERIFY(use);
        QVERIFY(!use->isEnabled());
        QVERIFY(find(app.window, "canvasViewRegionNote")->property("text").toString().contains("head"));
        // Zoom moves the rectangle with the picture.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(find(app.window, "canvasViewViewer")));
        QTest::keyClick(app.window, Qt::Key_1);
        QTRY_VERIFY(qAbs(find(app.window, "canvasViewRegion")->property("rectangle").toRectF().width() - 160) < 0.5);
        // A candidate: the button writes its revision, one undo step.
        click(app, "canvasViewChoice-" + shortOf(m_a));
        QTRY_VERIFY(find(app.window, "canvasViewUseRegion")->isEnabled());
        QCOMPARE(app.model.undoStack()->count(), 0); // looking is no edit
        click(app, "canvasViewUseRegion");
        QTRY_VERIFY(!find(app.window, "canvasViewUseRegion")->isEnabled());
        QCOMPARE(app.model.undoStack()->count(), 1);
        QCOMPARE(app.model.widgets()->regionState("region").value("revision").toString(), m_a);
        QVERIFY(find(app.window, "canvasViewRegionNote")->property("text").toString().contains(shortOf(m_a)));
        app.model.undoStack()->undo();
        QTRY_VERIFY(find(app.window, "canvasViewUseRegion")->isEnabled());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aSelectionThatIsNoRegionFollowsNothing() {
        App app(load(fixture(m_canvas)));
        QVERIFY(app.open());
        app.model.select({"text"});
        openDialog(app);
        QCOMPARE(model(app)->property("followNode").toString(), QString());
        QVERIFY(!find(app.window, "canvasViewRegionRow"));
        QVERIFY(!find(app.window, "canvasViewRegion"));
    }

    // Pictures: FUGUE_SHOT_DIR=<dir> test_canvas_view_window savesThePicturesOfTheWindow (nothing without the variable).
    void savesThePicturesOfTheWindow() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("FUGUE_SHOT_DIR is not set");
        App app(load(fixture(m_canvas)));
        QVERIFY(app.open());
        app.model.select({"region"});
        openDialog(app);
        click(app, "canvasViewChoice-" + shortOf(m_a));
        QTRY_VERIFY(find(app.window, "canvasViewRegion"));
        QTest::qWait(400);
        QVERIFY(test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/canvas-view-candidate.png"));
    }

    void aStoreWithNoCanvasSaysSo() {
        QDir(m_dir.filePath("home")).removeRecursively();
        QDir(m_dir.filePath("data")).removeRecursively();
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("file.canvas-view"));
        QTRY_VERIFY(dialog(app) && dialog(app)->property("opened").toBool());
        QTRY_VERIFY(find(app.window, "canvasViewEmpty"));
        QVERIFY(find(app.window, "canvasViewEmpty")->property("text").toString().contains("No canvas yet"));
    }
};

QTEST_MAIN(TestCanvasViewWindow)
#include "test_canvas_view_window.moc"
