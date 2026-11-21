// The side panels in the real window (Main.qml, offscreen, #56): N and W toggle the node library and
// the workflows panel and the View menu shows which is open; the library lists categories, filters,
// and adds a node by a click, the keys or a drop; the workflows panel lists the tabs and the recent
// files; the panel docks on either side, takes its width from the canvas, and is not made at all
// while closed.

#include <QFile>
#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"

using namespace canvas_test;

namespace {

const char *small = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [20, 100]}}}
})";

QPoint center(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

QQuickItem *rowItem(QQuickItem *list, int index) {
    QQuickItem *row = nullptr;
    QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, index));
    return row;
}

void typeText(QWindow *window, const QString &text) {
    for (const QChar c : text)
        QTest::keyClick(window, c.toLatin1());
}

QVariantList rowsOf(App &app) { return app.nodeLibrary.rows(); }

// The index of the first row of the library that is a category (or a node).
int firstRow(App &app, bool category) {
    const QVariantList rows = rowsOf(app);
    for (int i = 0; i < rows.size(); ++i)
        if (rows[i].toMap().value("category").toBool() == category)
            return i;
    return -1;
}

bool checked(App &app, const QString &id) { return app.commands.entry(id).value("checked").toBool(); }

} // namespace

class TestSidePanelsWindow : public QObject {
    Q_OBJECT

    static void click(App &app, QQuickItem *item) { QTest::mouseClick(app.window, Qt::LeftButton, {}, center(item)); }
    static QQuickItem *named(App &app, const QString &name) { return item(app.window->contentItem(), name); }
    static void settle() { QTest::qWait(30); }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void nothingIsMadeWhileClosed() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY(!named(app, "sidePanel"));
        QCOMPARE(app.canvas->x(), 0.0);
        QCOMPARE(app.canvas->width(), qreal(app.window->width()));
        QVERIFY(!checked(app, "panel.nodes"));
        QVERIFY(!checked(app, "panel.workflows"));
    }

    void nAndWToggleThePanelsAndTheViewMenuChecksTheOpenOne() {
        App app(load(small));
        QVERIFY(app.open());
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_N);
        QTRY_VERIFY(named(app, "nodeLibrary"));
        QVERIFY(checked(app, "panel.nodes"));
        QVERIFY(!checked(app, "panel.workflows"));
        QCOMPARE(app.sidePanels.tab(), QString("nodes"));
        // The canvas gave up the panel's width.
        QTRY_COMPARE(app.canvas->x(), qreal(app.sidePanels.width()));
        QCOMPARE(app.canvas->width(), app.window->width() - qreal(app.sidePanels.width()));
        // The keys went to the search field, so typing a letter does not toggle anything.
        QTRY_VERIFY(named(app, "nodeLibrarySearch")->hasActiveFocus());
        app.key(Qt::Key_W);
        app.key(Qt::Key_N);
        QCOMPARE(app.sidePanels.tab(), QString("nodes"));
        QCOMPARE(named(app, "nodeLibrarySearch")->property("text").toString(), QString("wn"));
        // From the canvas, W replaces the library with the workflows panel.
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_W);
        QTRY_VERIFY(named(app, "workflowsPanel"));
        QTRY_VERIFY(!named(app, "nodeLibrary"));
        QVERIFY(checked(app, "panel.workflows"));
        QVERIFY(!checked(app, "panel.nodes"));
        QTRY_COMPARE(app.canvas->x(), qreal(app.sidePanels.width()));
        // W again closes it, and nothing is left standing.
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_W);
        QTRY_VERIFY(!named(app, "sidePanel"));
        QTRY_COMPARE(app.canvas->x(), 0.0);
        QVERIFY(!checked(app, "panel.workflows"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theViewMenuTogglesThemToo() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("panel.nodes"));
        QTRY_VERIFY(named(app, "nodeLibrary"));
        QVERIFY(app.commands.trigger("panel.nodes"));
        QTRY_VERIFY(!named(app, "sidePanel"));
    }

    void aWindowStartsWithThePanelItClosedWith() {
        App app(load(small));
        app.sidePanels.setTab("workflows");
        QVERIFY(app.open());
        QVERIFY(named(app, "workflowsPanel"));
    }

    void theLibraryOpensACategoryAndFilters() {
        App app(load(small));
        QVERIFY(app.open());
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibraryList"));
        QQuickItem *list = named(app, "nodeLibraryList");
        const int closed = rowsOf(app).size();
        QVERIFY(closed > 0);
        QCOMPARE(firstRow(app, false) == -1 || firstRow(app, true) == 0, true);
        // A click on a category opens it.
        QQuickItem *category = rowItem(list, firstRow(app, true));
        QVERIFY(category);
        click(app, category);
        QTRY_VERIFY(rowsOf(app).size() > closed);
        named(app, "nodeLibrarySearch")->forceActiveFocus();
        // A query narrows to the matches, every category on the way open.
        typeText(app.window, "save");
        QTRY_COMPARE(app.nodeLibrary.query(), QString("save"));
        QVERIFY(app.nodeLibrary.nodeCount() > 0);
        QVERIFY(rowsOf(app).size() < closed + 40);
        typeText(app.window, "zzqq");
        QTRY_COMPARE(rowsOf(app).size(), 0);
        QVERIFY(named(app, "nodeLibraryEmpty")->isVisible());
        // Escape empties the field, then closes the panel.
        app.key(Qt::Key_Escape);
        QTRY_COMPARE(app.nodeLibrary.query(), QString());
        QVERIFY(named(app, "sidePanel"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!named(app, "sidePanel"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aClickOnANodeAddsItInTheMiddleOfTheViewAndUndoTakesItBack() {
        App app(load(small));
        QVERIFY(app.open());
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibraryList"));
        typeText(app.window, "save image");
        QTRY_VERIFY(app.nodeLibrary.nodeCount() > 0);
        QQuickItem *list = named(app, "nodeLibraryList");
        const int before = app.model.nodes()->rowCount();
        QTRY_VERIFY(rowItem(list, firstRow(app, false)));
        click(app, rowItem(list, firstRow(app, false)));
        QTRY_COMPARE(app.model.nodes()->rowCount(), before + 1);
        QVERIFY(app.commands.run("edit.undo"));
        QTRY_COMPARE(app.model.nodes()->rowCount(), before);
    }

    void theKeysAddANode() {
        App app(load(small));
        QVERIFY(app.open());
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibrarySearch")->hasActiveFocus());
        const int before = app.model.nodes()->rowCount();
        typeText(app.window, "save image");
        QTRY_VERIFY(app.nodeLibrary.nodeCount() > 0);
        app.key(Qt::Key_Return); // the first match, from the field
        QTRY_COMPARE(app.model.nodes()->rowCount(), before + 1);
        // Down goes to the list, Up from its first row back to the field (rows are walked one by one).
        app.key(Qt::Key_Down);
        QTRY_VERIFY(named(app, "nodeLibraryList")->hasActiveFocus());
        for (int i = 0; i < 20 && !named(app, "nodeLibrarySearch")->hasActiveFocus(); ++i)
            app.key(Qt::Key_Up);
        QTRY_VERIFY(named(app, "nodeLibrarySearch")->hasActiveFocus());
    }

    void aNodeDraggedOutLandsWhereItWasLetGo() {
        App app(load(small));
        QVERIFY(app.open());
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibraryList"));
        typeText(app.window, "save image");
        QTRY_VERIFY(app.nodeLibrary.nodeCount() > 0);
        QQuickItem *list = named(app, "nodeLibraryList");
        QTRY_VERIFY(rowItem(list, firstRow(app, false)));
        const int before = app.model.nodes()->rowCount();
        const QPoint from = center(rowItem(list, firstRow(app, false)));
        const QPoint to = app.at(app.canvas->width() * 0.6, app.canvas->height() * 0.6);
        app.drag(Qt::LeftButton, from, to);
        QTRY_COMPARE(app.model.nodes()->rowCount(), before + 1);
        // The new node's corner is at the drop point (in world coordinates).
        const QString id = app.model.nodes()->data(app.model.nodes()->index(before), NodeList::NodeId).toString();
        const QPointF dropped = app.canvas->mapFromScene(to);
        const QPointF world = QPointF((dropped.x() - app.pan().x()) / app.zoom(), (dropped.y() - app.pan().y()) / app.zoom());
        const QPointF got(app.model.nodes()->data(app.model.nodes()->index(before), NodeList::X).toReal(),
                          app.model.nodes()->data(app.model.nodes()->index(before), NodeList::Y).toReal());
        QVERIFY2(qAbs(got.x() - world.x()) < 2 && qAbs(got.y() - world.y()) < 2, qPrintable(id));
        // Let go over the panel itself: nothing is added.
        app.drag(Qt::LeftButton, from, from + QPoint(5, 40));
        QCOMPARE(app.model.nodes()->rowCount(), before + 1);
    }

    void theWorkflowsPanelListsTabsAndRecentFilesAndOpensThem() {
        App app(load(small));
        QVERIFY(app.open());
        // A file that was opened before: remembered, not open now.
        const QString path = app.dir.filePath("older.fugue");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(load(small).save());
        file.close();
        app.recentFiles.remember(path);
        app.workflows.newWorkflow();
        QCOMPARE(app.workflows.tabs().size(), 2);
        app.commands.run("panel.workflows");
        QTRY_VERIFY(named(app, "workflowsList"));
        QQuickItem *list = named(app, "workflowsList");
        // heading, 2 tabs, heading, 1 recent
        QCOMPARE(list->property("count").toInt(), 5);
        // The first tab is row 1: a click shows it.
        QCOMPARE(app.workflows.current(), 1);
        QTRY_VERIFY(rowItem(list, 1));
        click(app, rowItem(list, 1));
        QTRY_COMPARE(app.workflows.current(), 0);
        // The recent file opens in a tab of its own, and leaves the recent list.
        QTRY_VERIFY(rowItem(list, 4));
        click(app, rowItem(list, 4));
        QTRY_COMPARE(app.workflows.tabs().size(), 3);
        QTRY_COMPARE(list->property("count").toInt(), 5 + 1 - 1 + 1); // a tab more, the recent line says "empty"
        // The close button of a tab closes it.
        QQuickItem *close = item(rowItem(list, 3), "workflowsClose-3");
        QVERIFY(close);
        click(app, close);
        QTRY_COMPARE(app.workflows.tabs().size(), 2);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theGripChangesTheWidthAndTheDockButtonMovesThePanel() {
        App app(load(small));
        QVERIFY(app.open());
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "sidePanelGrip"));
        QQuickItem *grip = named(app, "sidePanelGrip");
        const int width = app.sidePanels.width();
        const QPoint from = center(grip);
        app.drag(Qt::LeftButton, from, from + QPoint(60, 0));
        QTRY_COMPARE(app.sidePanels.width(), width + 60);
        QTRY_COMPARE(app.canvas->x(), qreal(width + 60));
        // The keys do it too, and the width has bounds.
        grip->forceActiveFocus();
        app.key(Qt::Key_Left);
        QTRY_COMPARE(app.sidePanels.width(), width + 60 - 16);
        app.sidePanels.setWidth(10);
        QTRY_COMPARE(named(app, "sidePanel")->width(), qreal(SidePanelsMin()));
        // To the other side: the canvas is at the left edge, the panel at the right.
        click(app, named(app, "sidePanelDock"));
        QTRY_COMPARE(app.sidePanels.dock(), QString("right"));
        QTRY_COMPARE(app.canvas->x(), 0.0);
        QCOMPARE(app.canvas->width(), app.window->width() - qreal(app.sidePanels.width()));
        QCOMPARE(named(app, "sidePanel")->x(), app.window->width() - qreal(app.sidePanels.width()));
        // Dragging the grip to the left widens it there.
        const QPoint right = center(named(app, "sidePanelGrip"));
        const int wide = app.sidePanels.width();
        app.drag(Qt::LeftButton, right, right - QPoint(30, 0));
        QTRY_COMPARE(app.sidePanels.width(), wide + 30);
        // The close button closes it.
        click(app, named(app, "sidePanelClose"));
        QTRY_VERIFY(!named(app, "sidePanel"));
        QTRY_COMPARE(app.canvas->width(), qreal(app.window->width()));
    }

    static int SidePanelsMin() { return fugue::SidePanels::kMinWidth; }

    void theCanvasGivesUpTheSideItDocksOnRightToLeftToo() {
        QGuiApplication::setLayoutDirection(Qt::RightToLeft);
        {
            App app(load(small));
            app.sidePanels.setTab("nodes");
            QVERIFY(app.open());
            const qreal panel = app.sidePanels.width();
            QVERIFY(named(app, "sidePanel"));
            // The panel is on the left, so the canvas starts at its edge: the canvas never mirrors.
            QCOMPARE(named(app, "sidePanel")->x(), 0.0);
            QCOMPARE(app.canvas->x(), panel);
            QCOMPARE(app.canvas->width(), app.window->width() - panel);
            app.sidePanels.setDock("right");
            QTRY_COMPARE(app.canvas->x(), 0.0);
            QCOMPARE(app.canvas->width(), app.window->width() - panel);
            QCOMPARE(named(app, "sidePanel")->x(), app.window->width() - panel);
        }
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
    }
};

QTEST_MAIN(TestSidePanelsWindow)
#include "test_side_panels_window.moc"
