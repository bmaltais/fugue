// The window bar's and the area headers' layout rules (#262), at the menu model: which menus a view type's header
// holds (and that each is a real tree), and the workflow tab strip's widths and scroll. No window.
//
// Seams: menu/menus.h, menu/menu_layout_qml.h.

#include <QtTest>

#include "menu/menu_layout_qml.h"
#include "menu/menus.h"

using namespace fugue;

class TestMenuLayout : public QObject {
    Q_OBJECT
private slots:
    void aViewTypesHeaderHoldsItsOwnMenus() {
        const menus::ViewMenus graph = menus::viewMenus("graph");
        QVERIFY(menus::names().contains(graph.tree));
        QVERIFY(!graph.needsView);
        const menus::ViewMenus canvas = menus::viewMenus("canvas");
        QCOMPARE(canvas.tree, QString("canvas-view"));
        QVERIFY(canvas.needsView);
        QVERIFY(canvas.prefix != graph.prefix); // two bars of one window never share an object name
        QVERIFY(canvas.prefix != QString("canvas")); // nor with the File > Canvas… dialog's bar
        QVERIFY(menus::viewMenus("a-newer-view").tree.isEmpty());
        QCOMPARE(MenuLayout().viewMenus("canvas").value("tree").toString(), canvas.tree);
    }

    // The window bar holds only global menus: none of a view's menus are in it, and no view menu is global.
    void theWindowBarHoldsNoViewsMenus() {
        QStringList global;
        for (const QVariant &menu : menus::tree("bar"))
            global << menu.toMap().value("title").toString();
        QCOMPARE(global, (QStringList{"&File", "&Edit", "&Run", "&Window", "&Help"}));
        for (const char *view : {"graph", "canvas"})
            QVERIFY(!menus::tree(menus::viewMenus(view).tree).isEmpty());
        // The Canvas view's own Edit stays the view's: the window's Edit is the graph's, so each is told apart by its bar.
        QCOMPARE(menus::tree("canvas-view").first().toMap().value("title").toString(), QString("&Canvas"));
    }

    void tabsShrinkFromPreferredToMinimumThenTheStripScrolls() {
        QCOMPARE(menus::tabWidth(0, 500, 176, 64), 0.0);
        QCOMPARE(menus::tabWidth(2, 500, 176, 64), 176.0);   // they fit
        QCOMPARE(menus::tabWidth(4, 500, 176, 64), 125.0);   // they share the room
        QCOMPARE(menus::tabWidth(20, 500, 176, 64), 64.0);   // the least a tab is
        QCOMPARE(menus::tabWidth(3, 10, 176, 64), 64.0);     // no room at all
    }

    void theCurrentTabStaysInViewWithTheLeastMove() {
        // 10 tabs of 64 in 256: content 640, most offset 384.
        QCOMPARE(menus::tabScroll(0, 10, 64, 256, 0), 0.0);
        QCOMPARE(menus::tabScroll(3, 10, 64, 256, 0), 0.0);    // the last whole tab: stays
        QCOMPARE(menus::tabScroll(4, 10, 64, 256, 0), 64.0);   // one step to show it
        QCOMPARE(menus::tabScroll(1, 10, 64, 256, 200), 64.0); // brought back from the right
        QCOMPARE(menus::tabScroll(9, 10, 64, 256, 0), 384.0);
        QCOMPARE(menus::tabScroll(2, 3, 64, 256, 90), 0.0);    // all fit: no scroll, whatever it was
        QCOMPARE(menus::tabScroll(-1, 10, 64, 256, 9999), 384.0); // no current tab: only clamped
    }
};

QTEST_GUILESS_MAIN(TestMenuLayout)
#include "test_menu_layout.moc"
