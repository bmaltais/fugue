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
#include <QUndoStack>
#include <QtTest>

#include <thread>

#include "../canvas/canvas_app.h"
#include "../window_grab.h"
#include "engine/canvas_store.h"
#include "engine/templates.h"
#include "window_items.h"
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

    // #262: the Canvas view's menus are its area header's (as the Graph's are), not a bar inside the view; they act on the
    // view as its own bar did (its keys are live while one is open, its rows carry its state).
    void theCanvasViewsMenusAreItsAreaHeadersAndActOnTheView() {
        App app(load(withRegion(m_canvas)));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("area.split-column"));
        QTRY_VERIFY(find(app.window, "canvasHeaderMenuBar"));
        QVERIFY(!find(app.window, "canvasMenuBar")); // none inside the view
        QObject *view = find(app.window, "canvasArea")->property("view").value<QObject *>();
        QVERIFY(!view->property("ownMenus").toBool());
        QQuickItem *edit = nullptr;
        QTRY_VERIFY((edit = find(app.window, "canvasHeaderMenuTitle-Edit")) != nullptr);
        QTRY_VERIFY(sceneRect(edit).bottom() <= sceneRect(find(app.window, "areaHeader-2")).bottom()); // in the header
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(edit));
        QTRY_VERIFY(find(app.window, "menuItem-canvas-view.undo"));
        QVERIFY(view->property("menuOpen").toBool());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(edit)); // a second press closes it
        QTRY_VERIFY(!find(app.window, "menuItem-canvas-view.undo"));
        QVERIFY(!view->property("menuOpen").toBool());
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
        // The only Region node of this canvas is followed with nothing selected (#228), and stays followed.
        QTRY_COMPARE(backend->property("followNode").toString(), QString("region"));
        app.model.select({"region"});
        QTRY_COMPARE(backend->property("followNode").toString(), QString("region"));
        QTRY_VERIFY(find(app.window, "canvasViewRegion"));
        // The region was followed before the picture was read: the preview is computed again once it arrives.
        QTRY_VERIFY(find(app.window, "canvasViewContextLabel")->property("text").toString().contains("Context"));
        app.model.select({});
        QTest::qWait(50);
        QCOMPARE(backend->property("followNode").toString(), QString("region"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The owner's report (#228): with no Region node a drag on a Canvas area's picture draws (the press reaches the frame
    // in the area host), and Add a Canvas Region node keeps it.
    void aCanvasAreaDrawsAFreeRectangleWithNoRegionNode() {
        App app(load(blank));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("area.split-row"));
        QQuickItem *area = nullptr;
        QTRY_VERIFY((area = find(app.window, "canvasArea")) != nullptr);
        QObject *view = area->property("view").value<QObject *>();
        auto *viewer = view->property("viewer").value<QQuickItem *>();
        QTRY_VERIFY(viewer && viewer->isVisible() && viewer->width() > 50);
        QTRY_VERIFY(find(app.window, "canvasViewFrame"));
        settle();
        const QPoint from = viewer->mapToScene(QPointF(viewer->width() * 0.3, viewer->height() * 0.4)).toPoint();
        app.drag(Qt::LeftButton, from, from + QPoint(60, 40));
        QTRY_VERIFY(view->property("hasFreeRect").toBool());
        QTRY_VERIFY(find(app.window, "canvasViewAddRegion"));
        // The button's own call: at the foot of a short area the run toolbar floats over it (noted in the audit).
        QVERIFY(QMetaObject::invokeMethod(view, "addRegionNode"));
        QTRY_COMPARE(app.model.undoStack()->count(), 1);
        QTRY_VERIFY(!view->property("followNode").toString().isEmpty());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #239: a candidate added while a Canvas area is open on the head (by the command line) leaves the area where it is: no banner.
    void aCanvasAreaStaysOnTheHeadWhenACandidateArrives() {
        App app(load(blank));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("area.split-row"));
        QQuickItem *area = nullptr;
        QTRY_VERIFY((area = find(app.window, "canvasArea")) != nullptr);
        QObject *backend = area->property("view").value<QObject *>()->property("backend").value<QObject *>();
        QTRY_COMPARE(backend->property("canvasId").toString(), m_canvas);
        const QString head = backend->property("head").toString();
        QTRY_VERIFY(!head.isEmpty());
        const int before = backend->property("choices").toList().size();
        std::thread run([=] {
            QThread::msleep(300);
            PaintDocument base(256, 256);
            base.addLayer(LayerKind::Paint, "Base");
            base.addLayer(LayerKind::Paint, "Flower pot");
            canvas::CanvasStore::forSession().addRevision(m_canvas, base, head, "canvas-write:pot");
        });
        QTRY_VERIFY_WITH_TIMEOUT(backend->property("choices").toList().size() > before, 5000);
        run.join();
        QTest::qWait(300);
        QCOMPARE(backend->property("revisionB").toString(), head);
        QVERIFY(!find(app.window, "canvasViewNewResult"));
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

    // #486: the workflow is fitted for the canvas as it is when the opener finishes; at start the host has not laid the areas out
    // yet, so the fit is for the whole window and, in the Graph's half, the nodes land off to a side. It is fitted again for the
    // area's size until the person has moved the view.
    void aRestoredSplitFramesTheGraphInItsArea() {
        App app(load(blank));
        QFile file(app.dir.filePath("workspaces.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral(R"({"format": 1, "root": {"type": "branch", "axis": "row", "children": [
            {"size": 0.5, "node": {"type": "leaf", "area": 1, "view": "graph", "state": {}}},
            {"size": 0.5, "node": {"type": "leaf", "area": 2, "view": "canvas", "state": {"canvas": {"canvasId": "%1"}}}}]}})")
                       .arg(m_canvas).toUtf8());
        file.close();
        app.workspace.restore();
        QVERIFY(app.open());
        Template chosen;
        chosen.id = chosen.title = "z-image-turbo";
        chosen.folder = QStringLiteral(FUGUE_SOURCE_DIR "/templates/z-image-turbo");
        chosen.workflow = chosen.folder + "/workflow.fugue";
        QVERIFY(app.workflows.openTemplate(chosen));
        QTRY_VERIFY(host(app) && find(app.window, "canvasArea"));
        // The window's size is the compositor's to give: it comes after the opener has fitted the graph.
        app.window->resize(app.window->width() + 400, app.window->height() + 150);
        settle();
        QVERIFY(qAbs(app.canvas->width() - 0.5 * host(app)->width()) <= 1);
        const NodeList *nodes = app.model.nodes();
        QVERIFY(nodes->rowCount() > 1);
        const QRectF area(0, 0, app.canvas->width(), app.canvas->height());
        const qreal z = app.zoom();
        QVERIFY(z < 1);
        QRectF all;
        for (int i = 0; i < nodes->rowCount(); ++i) {
            const QModelIndex row = nodes->index(i);
            const QRectF box(app.world->x() + nodes->data(row, NodeList::X).toReal() * z,
                             app.world->y() + nodes->data(row, NodeList::Y).toReal() * z,
                             nodes->data(row, NodeList::Width).toReal() * z, nodes->data(row, NodeList::Height).toReal() * z);
            QVERIFY2(area.contains(box), qPrintable(nodes->data(row, NodeList::NodeId).toString() + " is inside the Graph area"));
            all = all.united(box);
        }
        // Framed, not just inside: the nodes sit in the middle of the area, as a single-area window frames them.
        QVERIFY2(qAbs(all.center().x() - area.center().x()) < area.width() * 0.1, qPrintable(QString::number(all.center().x()) + " of " + QString::number(area.width())));
        QVERIFY2(qAbs(all.center().y() - area.center().y()) < area.height() * 0.15, qPrintable(QString::number(all.center().y()) + " of " + QString::number(area.height())));
        // A view the person has moved is kept when the window is resized.
        QVERIFY(QMetaObject::invokeMethod(app.canvas, "panBy", Q_ARG(QVariant, 40), Q_ARG(QVariant, 0)));
        const QPointF at(app.world->x(), app.world->y());
        app.window->resize(app.window->width() - 200, app.window->height());
        settle();
        QCOMPARE(QPointF(app.world->x(), app.world->y()), at);
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

    // #228 (docs/ui-ergonomics.md D5): a Canvas area with the focus keeps the Graph's keys from the Graph behind it, and
    // the window's bare Q from quitting; Home, + and the rest are its picture's. Ctrl+Q still quits, Escape is the run's
    // unless a rectangle drag is on, and the Graph has its keys back once it is pressed.
    void aFocusedCanvasAreaKeepsTheGraphsKeysAndQ() {
        App app(load(withRegion(m_canvas)));
        QVERIFY(app.open());
        app.commands.setUnsavedWork(true); // quit asks first: the question is the sign that the key reached it
        QSignalSpy quits(&app.commands, &CommandRegistry::quitConfirmationRequested);
        int cancels = 0;
        QVERIFY(app.commands.setHandler("run.cancel", [&cancels] { ++cancels; }));
        app.commands.setContextActive("run", true); // a run is going
        QVERIFY(app.commands.trigger("area.split-row"));
        QQuickItem *area = nullptr;
        QTRY_VERIFY((area = find(app.window, "canvasArea")) != nullptr);
        QObject *view = area->property("view").value<QObject *>();
        auto *viewer = view->property("viewer").value<QQuickItem *>();
        QTRY_VERIFY(viewer && viewer->isVisible() && viewer->width() > 50);
        app.model.select({"region"});
        QTRY_VERIFY(find(app.window, "canvasViewFrame"));
        settle();
        QVERIFY(app.commands.trigger("view.zoom-out")); // the Graph zoomed (out: framing the Region lands near 1.1, so zooming in could meet it), so a fit of it would show
        const qreal graphZoom = app.zoom();
        // A press on the picture (clear of the border's 24px target): the area is active and its view has the keys.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, viewer->mapToScene(QPointF(40, 40)).toPoint());
        QTRY_COMPARE(app.workspace.active(), 2);
        app.key(Qt::Key_1);
        QTRY_VERIFY(viewer->property("actual").toBool());
        app.key(Qt::Key_Home); // the picture fits, not the Graph
        QTRY_VERIFY(!viewer->property("actual").toBool());
        app.key(Qt::Key_Period);
        app.key(Qt::Key_Plus);
        QTRY_VERIFY(viewer->property("zoom").toReal() > 1);
        app.key(Qt::Key_Delete);
        app.key(Qt::Key_A, Qt::ControlModifier);
        app.commands.setUnsavedWork(true);
        app.key(Qt::Key_Q); // Quick Mask (#230), never Quit; again leaves it and gives the region tool back
        QTRY_VERIFY(view->property("quickMask").toBool());
        app.key(Qt::Key_Q);
        QTRY_VERIFY(!view->property("quickMask").toBool());
        QCOMPARE(view->property("tool").toString(), QString("region"));
        QCOMPARE(app.zoom(), graphZoom);
        QVERIFY(app.model.document().graph.nodes.contains("region"));
        QCOMPARE(quits.count(), 0);
        app.commands.setUnsavedWork(true);
        app.key(Qt::Key_Q, Qt::ControlModifier);
        QTRY_COMPARE(quits.count(), 1);
        app.key(Qt::Key_Escape); // closes the quit question (unsaved work)
        QTRY_VERIFY(!app.window->findChild<QObject *>("quitAsk") || !app.window->findChild<QObject *>("quitAsk")->property("visible").toBool());
        QCOMPARE(cancels, 0);
        // Escape: the run's, but the drag's while one is on.
        app.key(Qt::Key_Escape);
        QTRY_COMPARE(cancels, 1);
        app.key(Qt::Key_0);
        settle();
        // On the picture itself (the tool strip narrows the view, so the fitted picture is letterboxed: its middle row).
        const QPoint from = viewer->mapToScene(QPointF(viewer->width() * 0.3, viewer->height() * 0.5)).toPoint();
        QTest::mouseMove(app.window, from);
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        for (int i = 1; i <= 4; ++i)
            QTest::mouseMove(app.window, from + QPoint(10 * i, 6 * i));
        QTRY_VERIFY(find(app.window, "canvasViewFrame")->property("dragging").toBool());
        app.key(Qt::Key_Escape);
        QVERIFY(!find(app.window, "canvasViewFrame")->property("dragging").toBool());
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, from + QPoint(40, 24));
        QCOMPARE(cancels, 1);
        // The Graph pressed: its keys are its own again.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.canvas->mapToScene(QPointF(app.canvas->width() - 20, app.canvas->height() - 20)).toPoint());
        QTRY_COMPARE(app.workspace.active(), 1);
        app.key(Qt::Key_Period);
        QTRY_VERIFY(app.zoom() != graphZoom);
        app.commands.setContextActive("run", false);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Pictures: FUGUE_SHOT_DIR=<dir> test_area_split_window savesThePictures (nothing without it), dark and light: two
    // areas with their headers and border, a Canvas area following a region, and the view-type menu open.
    void savesThePictures_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("name");
        QTest::newRow("dark") << "tokyo-night" << "dark";
        QTest::newRow("light") << "catppuccin-latte" << "light";
    }
    void savesThePictures() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("FUGUE_SHOT_DIR is not set");
        QFETCH(QString, theme);
        QFETCH(QString, name);
        App app(load(withRegion(m_canvas)), {}, Look{theme, 8});
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("area.split-row"));
        QTRY_VERIFY(find(app.window, "canvasArea"));
        app.model.select({"region"});
        QTRY_VERIFY(find(app.window, "canvasViewFrame"));
        const auto save = [&](const QString &what) {
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/areas-%2-%3.png").arg(QString::fromLocal8Bit(dir), name, what)));
        };
        save("canvas");
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(find(app.window, "areaType-2")));
        QTRY_VERIFY(app.window->findChild<QObject *>("areaViewMenu"));
        save("type-menu");
        app.key(Qt::Key_Escape);
        // #262: the Canvas area's own menus in its header, one open; then the window bar with workflow tabs in its row.
        QQuickItem *layer = nullptr;
        QTRY_VERIFY((layer = find(app.window, "canvasHeaderMenuTitle-Layer")) != nullptr);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(layer));
        QTRY_VERIFY(find(app.window, "menuItem-canvas-view.new-layer"));
        save("canvas-header-menu");
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(layer));
        app.workflows.dismissPendingTrust();
        app.workflows.dismissNotice();
        for (int i = 0; i < 3; ++i)
            app.workflows.newWorkflow();
        QTRY_VERIFY(find(app.window, "tab-3"));
        save("window-bar-tabs");
    }

    // #228: in a narrow Canvas area the layers panel gives the picture most of the width (a quarter of the view, never under 32u).
    void theLayersPanelIsNarrowInANarrowArea() {
        App app(load(withRegion(m_canvas)));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("area.split-row"));
        QQuickItem *area = nullptr;
        QTRY_VERIFY((area = find(app.window, "canvasArea")) != nullptr);
        QQuickItem *panel = nullptr;
        QTRY_VERIFY((panel = find(app.window, "canvasLayersPanel")) != nullptr);
        const qreal u = 4, view = area->width();
        QCOMPARE(panel->width(), qBound(32 * u, view / 4, 56 * u));
        QVERIFY(panel->width() <= 56 * u);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #226, last because it edits the shared canvas: over a Canvas area's layers panel the keys are the panel's (the
    // window's own shortcuts, the `canvas-layers` context), so Delete deletes the layer and not the selected node.
    void theLayersPanelOwnsTheKeysUnderThePointer() {
        App app(load(withRegion(m_canvas)));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("area.split-row"));
        QQuickItem *area = nullptr;
        QTRY_VERIFY((area = find(app.window, "canvasArea")) != nullptr);
        QObject *backend = area->property("view").value<QObject *>()->property("backend").value<QObject *>();
        QTRY_VERIFY(!backend->property("layers").toList().isEmpty());
        const int count = backend->property("layers").toList().size(); // the earlier tests' layers; the top one is current
        app.model.select({"region"});
        QTRY_VERIFY(find(app.window, "canvasLayerList"));
        settle();
        QTest::mouseMove(app.window, centerOf(find(app.window, "canvasLayerList")));
        QTRY_VERIFY(app.commands.contextActive("canvas-layers"));
        QCOMPARE(app.commands.ownerOf("Del"), QString("canvas-layers.delete"));
        QTest::keyClick(app.window, Qt::Key_H);
        QTRY_VERIFY(!backend->property("layers").toList().at(0).toMap().value("visible").toBool());
        QTest::keyClick(app.window, Qt::Key_Delete);
        QTRY_COMPARE(backend->property("layers").toList().size(), count - 1);
        QVERIFY(app.model.document().graph.nodes.contains("region")); // the node is not the one deleted
        QTest::keyClick(app.window, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(backend->property("layers").toList().size(), count);
        // Away from the panel the Graph has its keys back.
        QTest::mouseMove(app.window, centerOf(app.canvas));
        QTRY_VERIFY(!app.commands.contextActive("canvas-layers"));
        QCOMPARE(app.commands.ownerOf("Del"), QString("edit.delete"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

FUGUE_TEST_MAIN(TestAreaSplitWindow, QGuiApplication)
#include "test_area_split_window.moc"
