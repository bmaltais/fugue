// The window's areas (#217 slice 1, spec 12.16) in the real window, offscreen, with QTest's mouse and key events
// only (no window outside the test process, no real pointer): a split makes the body's host and moves the Graph
// into it beside a Canvas area, each area with a header; a border drag, a double click and the arrow keys resize;
// the type menu moves the Graph and the commands move it back; a Canvas area follows the selected Canvas Region and
// keeps its canvas; closing back to one area gives the default window again, item for item; the layout comes back
// at start, and a damaged file gives the default window and is kept as .bad. The canvas store is the test's own
// (FUGUE_HOME and the XDG folders point at a temporary folder).

#include <QDir>
#include <QPointer>
#include <QQuickStyle>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <thread>

#include "../canvas/canvas_app.h"
#include "engine/canvas_store.h"
#include "window_items.h"

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

int countNamed(QQuickItem *from, const QString &name) {
    int count = 0;
    for (QQuickItem *child : from->childItems())
        count += (child->objectName() == name ? 1 : 0) + countNamed(child, name);
    return count;
}

QPoint centerOf(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }
QRectF sceneRect(QQuickItem *item) { return QRectF(item->mapToScene(QPointF(0, 0)), QSizeF(item->width(), item->height())); }

const char *blank = R"({"fugue": 1, "graph": {"nodes": {}}})";

QByteArray withRegion(const QString &canvas) {
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {"region": {"type": "fugue/canvas-region", "version": "1.0.0",
                                 "inputs": {"canvas": "%1", "x": 100, "y": 60, "width": 160, "height": 96, "padding": 20}}}},
  "layout": {"nodes": {"region": {"pos": [20, 20]}}, "view": {"center": [200, 200], "zoom": 1}}
})").arg(canvas).toUtf8();
}

} // namespace

class TestAreaSplitWindow : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_canvas;

    QQuickItem *host(App &app) { return find(app.window, "areaHost"); }
    // Waits for the window to settle after a layout change (the host is laid out by the column).
    void settle() { QTest::qWait(60); }

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
        canvas::CanvasStore store = canvas::CanvasStore::forSession();
        PaintDocument base(256, 256);
        base.addLayer(LayerKind::Paint, "Base");
        base.brush.color = Rgba{30, 60, 200, 255};
        base.fillRect(0, 0, 256, 256);
        const auto made = store.create("Sketch", base, "new");
        QVERIFY(made);
        m_canvas = made->id;
    }

    void aSplitMakesTheHostAndClosingGivesTheDefaultWindowBack() {
        App app(load(blank));
        QVERIFY(app.open());
        QTest::qWait(200);
        const QString before = window_items::describe(app.window->contentItem());
        QVERIFY(!host(app));
        app.key(Qt::Key_Backslash, Qt::ControlModifier); // area.split-row
        QTRY_VERIFY(host(app));
        QCOMPARE(app.workspace.count(), 2);
        QCOMPARE(app.canvas->parentItem(), host(app));
        QTRY_VERIFY(find(app.window, "canvasArea"));
        QVERIFY(find(app.window, "areaHeader-1") && find(app.window, "areaHeader-2"));
        QCOMPARE(countNamed(host(app), "areaHandle"), 1);
        settle();
        // The Graph at the start, under its header; the Canvas at the end; together the body.
        QQuickItem *canvasArea = find(app.window, "canvasArea");
        const QRectF body = sceneRect(host(app));
        const QRectF graph = sceneRect(app.canvas);
        const QRectF other = sceneRect(canvasArea);
        const qreal headerHeight = find(app.window, "areaHeader-1")->height();
        QVERIFY(headerHeight >= 24);
        QCOMPARE(graph.left(), body.left());
        QCOMPARE(graph.top(), body.top() + headerHeight);
        QCOMPARE(graph.right(), other.left());
        QCOMPARE(other.right(), body.right());
        QVERIFY(qAbs(graph.width() - body.width() / 2) <= 1);
        // The side strip runs along the body, as it ran along the canvas.
        QQuickItem *strip = find(app.window, "sideTabStrip");
        QVERIFY(strip);
        QCOMPARE(sceneRect(strip).top(), body.top());
        QCOMPARE(strip->height(), body.height());
        QCOMPARE(app.workspace.bodySize(), body.size());
        // The new area is the active one; its header's close button closes it.
        QCOMPARE(app.workspace.active(), 2);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(find(app.window, "areaClose-2")));
        QTRY_VERIFY(!host(app));
        QCOMPARE(app.workspace.count(), 1);
        QVERIFY(QByteArray(app.canvas->parentItem()->metaObject()->className()).contains("ColumnLayout"));
        QTest::qWait(200); // the host and the views it held are destroyed later
        QCOMPARE(window_items::describe(app.window->contentItem()), before);
        QCOMPARE(window_items::describe(app.window->contentItem()), window_items::expected());
        QVERIFY(!QFile::exists(app.dir.filePath("workspaces.json")));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theBorderDragsAndDoubleClicksAndTakesTheArrowKeys() {
        App app(load(blank));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("area.split-row"));
        QTRY_VERIFY(host(app) && find(app.window, "areaHandle"));
        settle();
        QQuickItem *handle = find(app.window, "areaHandle");
        const qreal width = app.canvas->width();
        const QPoint from = centerOf(handle);
        app.drag(Qt::LeftButton, from, from + QPoint(100, 0));
        QTRY_VERIFY(qAbs(app.canvas->width() - (width + 100)) <= 1);
        QVERIFY(qAbs(centerOf(handle).x() - (from.x() + 100)) <= 1); // the border is under the pointer
        // Past the Canvas's minimum, it stops there.
        app.drag(Qt::LeftButton, centerOf(handle), QPoint(app.window->width() - 2, from.y()));
        QTRY_VERIFY(qAbs(find(app.window, "canvasArea")->width() - 240) <= 1);
        // A double click shares the row equally.
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, centerOf(handle));
        QTRY_VERIFY(qAbs(app.canvas->width() - host(app)->width() / 2) <= 1);
        // The arrow keys move it once it has the focus (a press gives it).
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(handle));
        QTRY_VERIFY(handle->hasActiveFocus());
        const qreal now = app.canvas->width();
        app.key(Qt::Key_Right);
        QTRY_VERIFY(qAbs(app.canvas->width() - (now + 8)) <= 1);
        app.key(Qt::Key_Left, Qt::ShiftModifier);
        QTRY_VERIFY(qAbs(app.canvas->width() - (now - 32)) <= 1);
        // Splitting the Graph's area leaves the Canvas area's view as it was: the same item.
        const QPointer<QQuickItem> kept = find(app.window, "canvasArea");
        app.workspace.setActive(1);
        QVERIFY(app.commands.trigger("area.split-column"));
        QTRY_COMPARE(countNamed(host(app), "canvasArea"), 2);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(kept);
        QCOMPARE(kept->property("areaId").toInt(), 2);
        QCOMPARE(countNamed(host(app), "areaHandle"), 2);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theTypeMenuMovesTheGraphAndTheCommandsMoveItBack() {
        App app(load(blank));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("area.split-column"));
        QTRY_VERIFY(find(app.window, "areaType-2"));
        settle();
        QCOMPARE(app.workspace.viewOf(2), QString("canvas"));
        // Area 2's menu: Graph moves here.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(find(app.window, "areaType-2")));
        QObject *menu = nullptr;
        QTRY_VERIFY((menu = app.window->findChild<QObject *>("areaViewMenu")) && menu->property("opened").toBool());
        app.key(Qt::Key_Up);
        app.key(Qt::Key_Return);
        QTRY_COMPARE(app.workspace.graphArea(), 2);
        QCOMPARE(app.workspace.viewOf(1), QString("canvas"));
        settle();
        QTRY_VERIFY(find(app.window, "canvasArea"));
        QVERIFY(sceneRect(find(app.window, "canvasArea")).bottom() <= sceneRect(app.canvas).top()); // the Canvas on top now
        // Area 1 (the Canvas) made active by a press in it, then Ctrl+Alt+G brings the Graph back there.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(find(app.window, "canvasArea")));
        QTRY_COMPARE(app.workspace.active(), 1);
        app.key(Qt::Key_G, Qt::ControlModifier | Qt::AltModifier);
        QTRY_COMPARE(app.workspace.graphArea(), 1);
        // The Graph's area cannot show a Canvas: the key does nothing there.
        QTRY_COMPARE(app.workspace.active(), 1);
        app.key(Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(app.workspace.graphArea(), 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aCanvasAreaFollowsTheSelectedRegionAndKeepsItsCanvas() {
        App app(load(withRegion(m_canvas)));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("area.split-row"));
        QQuickItem *area = nullptr;
        QTRY_VERIFY((area = find(app.window, "canvasArea")) != nullptr);
        QObject *backend = area->property("view").value<QObject *>()->property("backend").value<QObject *>();
        QTRY_COMPARE(backend->property("canvasId").toString(), m_canvas); // the newest
        QTRY_COMPARE(app.workspace.areaState(2).value("canvasId").toString(), m_canvas);
        QCOMPARE(backend->property("followNode").toString(), QString());
        app.model.select({"region"});
        QTRY_COMPARE(backend->property("followNode").toString(), QString("region"));
        QTRY_VERIFY(find(app.window, "canvasViewRegion"));
        // The region was followed before the picture was read: the preview is computed again once it arrives.
        QTRY_VERIFY(find(app.window, "canvasViewRegionInfo")->property("text").toString().contains("Context"));
        app.model.select({});
        QTRY_COMPARE(backend->property("followNode").toString(), QString());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // A run adds a candidate (as Canvas Write does) while a Canvas area is open on the head: the area shows it with its banner.
    void aCanvasAreaShowsANewResultOfARunAndTheBannerDecides() {
        App app(load(blank));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("area.split-row"));
        QQuickItem *area = nullptr;
        QTRY_VERIFY((area = find(app.window, "canvasArea")) != nullptr);
        QObject *backend = area->property("view").value<QObject *>()->property("backend").value<QObject *>();
        QTRY_COMPARE(backend->property("canvasId").toString(), m_canvas);
        const QString head = backend->property("head").toString();
        QTRY_VERIFY(!head.isEmpty());
        QVERIFY(!find(app.window, "canvasViewNewResult"));
        std::thread run([=] {
            QThread::msleep(300);
            PaintDocument base(256, 256);
            base.addLayer(LayerKind::Paint, "Base");
            base.addLayer(LayerKind::Paint, "Flower pot");
            canvas::CanvasStore::forSession().addRevision(m_canvas, base, head, "canvas-write:pot");
        });
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "canvasViewNewResult"), 5000);
        run.join();
        const QString shown = backend->property("revisionB").toString();
        QVERIFY(shown != head);
        QCOMPARE(backend->property("newResult").toString(), shown);
        QVERIFY(find(app.window, "canvasViewNewResultText")->property("text").toString().contains("pot"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(find(app.window, "canvasViewNewAccept")));
        QTRY_COMPARE(backend->property("head").toString(), shown);
        QTRY_VERIFY(!find(app.window, "canvasViewNewResult"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theLayoutComesBackAtStart() {
        App app(load(blank));
        QFile file(app.dir.filePath("workspaces.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral(R"({"format": 1, "root": {"type": "branch", "axis": "row", "children": [
            {"size": 0.7, "node": {"type": "leaf", "area": 1, "view": "graph", "state": {}}},
            {"size": 0.3, "node": {"type": "leaf", "area": 2, "view": "canvas", "state": {"canvas": {"canvasId": "%1"}}}}]}})")
                       .arg(m_canvas).toUtf8());
        file.close();
        app.workspace.restore();
        QVERIFY(app.open());
        QTRY_VERIFY(host(app) && find(app.window, "canvasArea"));
        settle();
        QObject *backend = find(app.window, "canvasArea")->property("view").value<QObject *>()->property("backend").value<QObject *>();
        QCOMPARE(backend->property("canvasId").toString(), m_canvas);
        QVERIFY(qAbs(app.canvas->width() - 0.7 * host(app)->width()) <= 1);
        // A change is written a moment later.
        const QByteArray was = [&] { QFile f(file.fileName()); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }();
        QVERIFY(app.commands.trigger("area.split-column"));
        QTRY_VERIFY([&] { QFile f(file.fileName()); return f.open(QIODevice::ReadOnly) && f.readAll() != was; }());
        QCOMPARE(AreaTree::load([&] { QFile f(file.fileName()); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }()).count(), 3);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aDamagedLayoutGivesTheDefaultWindowAndIsKept() {
        App app(load(blank));
        QFile file(app.dir.filePath("workspaces.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"format\": 1, \"root\": [");
        file.close();
        QObject::connect(&app.workspace, &Workspace::message, &app.workflows, &Workflows::notify);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^workspace: .*workspaces.json not a JSON object"));
        app.workspace.restore();
        QVERIFY(app.open());
        QTest::qWait(200);
        QVERIFY(!host(app));
        QVERIFY(QFile::exists(app.dir.filePath("workspaces.json.bad")));
        QVERIFY(!QFile::exists(app.dir.filePath("workspaces.json")));
        QVERIFY(app.workflows.notice().contains("workspaces.json.bad"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void focusModeHidesTheHeadersAndTheViewsTakeTheirPlace() {
        App app(load(blank));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("area.split-row"));
        QTRY_VERIFY(find(app.window, "areaHeader-1"));
        settle();
        QVERIFY(app.canvas->y() > 0);
        QVERIFY(app.commands.trigger("view.focus-mode"));
        QTRY_VERIFY(!find(app.window, "areaHeader-1"));
        QTRY_COMPARE(app.canvas->y(), 0.0);
        QVERIFY(find(app.window, "areaHandle")); // the border stays
        QVERIFY(app.commands.trigger("view.focus-mode"));
        QTRY_VERIFY(find(app.window, "areaHeader-1"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theAreaKeysHaveNoOtherOwner() {
        App app(load(blank));
        for (const auto &[key, command] : {std::pair{"Ctrl+\\", "area.split-row"}, std::pair{"Ctrl+Alt+\\", "area.split-column"},
                                           std::pair{"Ctrl+Alt+W", "area.close"}, std::pair{"Ctrl+Alt+G", "area.view-graph"},
                                           std::pair{"Ctrl+Alt+C", "area.view-canvas"}}) {
            int owners = 0;
            for (const QString &id : app.commands.ids())
                owners += app.commands.keys(id).contains(QString::fromLatin1(key)) ? 1 : 0;
            QVERIFY2(owners == 1, key);
            QCOMPARE(app.commands.keys(QString::fromLatin1(command)), QStringList({QString::fromLatin1(key)}));
        }
    }
};

QTEST_MAIN(TestAreaSplitWindow)
#include "test_area_split_window.moc"
