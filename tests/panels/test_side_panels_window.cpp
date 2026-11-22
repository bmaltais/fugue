// The side panels in the real window (Main.qml, offscreen, #56): N and W toggle the node library and
// the workflows panel and the View menu shows which is open; the library lists categories, filters,
// and adds a node by a click, the keys or a drop; the workflows panel lists the tabs and the recent
// files; the panel docks on either side, takes its width from the canvas, and is not made at all
// while closed.

#include <QFile>
#include <QJsonDocument>
#include <QQuickStyle>
#include <QtTest>

#include "../scroll_checks.h"
#include "../test_home_guard.h"
#include "../canvas/canvas_app.h"
#include "engine/group_library.h"

using namespace canvas_test;
using scroll_checks::wheelAt;

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

// The row of the library list showing `key`, or null; the trash can in a row, or null.
QQuickItem *rowOfKey(App &app, QQuickItem *list, const QString &key) {
    const QVariantList rows = rowsOf(app);
    for (int i = 0; i < rows.size(); ++i)
        if (rows[i].toMap().value("key").toString() == key) {
            QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, i), Q_ARG(int, 0)); // ListView.Beginning
            return rowItem(list, i);
        }
    return nullptr;
}


} // namespace

class TestSidePanelsWindow : public QObject {
    Q_OBJECT

    static void click(App &app, QQuickItem *item) { QTest::mouseClick(app.window, Qt::LeftButton, {}, center(item)); }
    static QQuickItem *named(App &app, const QString &name) { return item(app.window->contentItem(), name); }
    // The tab strip's width (#159): the canvas starts after it, and so does a panel docked on the left.
    static qreal stripWidth(App &app) { return named(app, "sideTabStrip") ? named(app, "sideTabStrip")->width() : 0; }
    static void settle() { QTest::qWait(30); }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }
    // The test's home lasts between runs (build/test-home/), so groups an earlier run saved to the
    // library would still be there: a group's key would be local/group-10, and the tests that count
    // library rows would see more than their own. Each test starts with an empty library.
    void init() {
        FUGUE_REQUIRE_TEST_HOME(fugue::libraryDir());
        QDir library(fugue::libraryDir());
        for (const QString &group : library.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            QDir(library.filePath(group)).removeRecursively();
    }

    void nothingIsMadeWhileClosed() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY(!named(app, "sidePanel"));
        QCOMPARE(app.canvas->x(), stripWidth(app));
        QCOMPARE(app.canvas->width(), app.window->width() - stripWidth(app));
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
        QTRY_COMPARE(app.canvas->x(), stripWidth(app) + app.sidePanels.width());
        QCOMPARE(app.canvas->width(), app.window->width() - stripWidth(app) - qreal(app.sidePanels.width()));
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
        QTRY_COMPARE(app.canvas->x(), stripWidth(app) + app.sidePanels.width());
        // W again closes it, and nothing is left standing.
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_W);
        QTRY_VERIFY(!named(app, "sidePanel"));
        QTRY_COMPARE(app.canvas->x(), stripWidth(app));
        QVERIFY(!checked(app, "panel.workflows"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The tab strip (#159): an icon per registered panel on the left edge, always there.
    void theStripHasATabPerPanelAndIsThereWithNothingOpen() {
        App app(load(small));
        QVERIFY(app.open());
        QQuickItem *strip = named(app, "sideTabStrip");
        QVERIFY(strip);
        QCOMPARE(strip->x(), 0.0);
        QCOMPARE(app.canvas->x(), strip->width()); // the canvas starts at the strip's edge
        const QVariantList panels = app.sidePanels.panels();
        QCOMPARE(panels.size(), 4); // two panels, Templates (#192) and Settings
        for (const QVariant &panel : panels) {
            QQuickItem *tab = named(app, "sideTab-" + panel.toMap().value("tab").toString());
            QVERIFY(tab);
            // The floors: a 36px hit area, a 28px glyph box.
            QVERIFY(tab->width() >= 36);
            QVERIFY(tab->height() >= 36);
            QVERIFY(tab->activeFocusOnTab());
            QVERIFY(tab->mapToItem(strip, QPointF(0, 0)).x() >= 0);
        }
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aClickOnATabOpensItAndASecondClickClosesItInStepWithTheKeysAndTheMenu() {
        App app(load(small));
        QVERIFY(app.open());
        click(app, named(app, "sideTab-nodes"));
        QTRY_VERIFY(named(app, "nodeLibrary"));
        QVERIFY(checked(app, "panel.nodes"));
        QVERIFY(named(app, "sideTab-nodes")->property("open").toBool());
        QVERIFY(!named(app, "sideTab-workflows")->property("open").toBool());
        // The panel sits beside the strip, and the canvas beside the panel.
        const qreal strip = named(app, "sideTabStrip")->width();
        QTRY_COMPARE(named(app, "sidePanel")->x(), strip);
        QTRY_COMPARE(app.canvas->x(), strip + app.sidePanels.width());
        // The search field has the focus: a click on a tab asks for it, as the key does.
        QTRY_VERIFY(named(app, "nodeLibrarySearch")->hasActiveFocus());
        // Another tab replaces it.
        click(app, named(app, "sideTab-workflows"));
        QTRY_VERIFY(named(app, "workflowsPanel"));
        QTRY_VERIFY(!named(app, "nodeLibrary"));
        QVERIFY(checked(app, "panel.workflows"));
        QVERIFY(!checked(app, "panel.nodes"));
        QTRY_VERIFY(named(app, "sideTab-workflows")->property("open").toBool());
        QVERIFY(!named(app, "sideTab-nodes")->property("open").toBool());
        // The same tab again closes it; the strip stays.
        click(app, named(app, "sideTab-workflows"));
        QTRY_VERIFY(!named(app, "sidePanel"));
        QVERIFY(named(app, "sideTabStrip"));
        QTRY_COMPARE(app.canvas->x(), strip);
        QVERIFY(!named(app, "sideTab-workflows")->property("open").toBool());
        // The key and the menu command move the marks too.
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_N);
        QTRY_VERIFY(named(app, "sideTab-nodes")->property("open").toBool());
        QVERIFY(app.commands.trigger("panel.nodes"));
        QTRY_VERIFY(!named(app, "sideTab-nodes")->property("open").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aTabIsReachedByTabAndPressedByEnterAndSpace() {
        App app(load(small));
        QVERIFY(app.open());
        QQuickItem *tab = named(app, "sideTab-workflows");
        tab->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(tab->hasActiveFocus());
        app.key(Qt::Key_Return);
        QTRY_VERIFY(named(app, "workflowsPanel"));
        tab->forceActiveFocus(Qt::TabFocusReason);
        app.key(Qt::Key_Space);
        QTRY_VERIFY(!named(app, "sidePanel"));
        // It carries its name for the screen reader, and its tooltip names the key.
        QCOMPARE(tab->property("tip").toString(), QString("Workflows (W)"));
    }

    // The Templates action tab (#192): directly under the Workflows tab, it runs its command and
    // is never a checked panel; a click, Enter and Space all open the dialog.
    void theTemplatesActionTabOpensTheDialogAndIsNeverChecked() {
        App app(load(small));
        QVERIFY(app.open());
        QQuickItem *strip = named(app, "sideTabStrip");
        QQuickItem *tab = named(app, "sideTab-templates");
        QVERIFY(strip && tab);
        QVERIFY(tab->isVisible());
        QVERIFY(tab->width() >= 36 && tab->height() >= 36);
        QVERIFY(tab->activeFocusOnTab());
        // Directly under the Workflows tab (touching, as the panel tabs do), above the strip's rule.
        const qreal templatesTop = tab->mapToItem(strip, QPointF(0, 0)).y();
        const QQuickItem *workflows = named(app, "sideTab-workflows");
        QCOMPARE(templatesTop, workflows->mapToItem(strip, QPointF(0, 0)).y() + workflows->height());
        QVERIFY(templatesTop + tab->height() <= named(app, "sideTabActionsRule")->mapToItem(strip, QPointF(0, 0)).y());
        // The panel registry still gives one panel tab per panel, and the action is not one.
        const QVariantList list = app.sidePanels.panels();
        QCOMPARE(list.size(), 4);
        QCOMPARE(list.at(2).toMap().value("kind").toString(), QString("action"));
        QCOMPARE(list.at(2).toMap().value("command").toString(), QString("file.templates"));
        QCOMPARE(list.at(0).toMap().value("kind").toString(), QString("panel"));
        QVERIFY(!tab->property("open").toBool());
        QVERIFY(tab->property("isAction").toBool());
        // The tooltip names the command's key, when it has one, as the other tabs do.
        const QStringList keys = app.commands.entry("file.templates").value("keys").toStringList();
        const QString name = list.at(2).toMap().value("name").toString();
        QCOMPARE(tab->property("tip").toString(), keys.isEmpty() ? name : QString("%1 (%2)").arg(name, keys.first()));
        QCOMPARE(name, QString("Templates"));

        const auto dialog = [&app]() { return app.window->findChild<QObject *>("templatesDialog"); };
        const auto isOpen = [&]() { return dialog() && dialog()->property("opened").toBool(); };
        const auto close = [&]() {
            QMetaObject::invokeMethod(dialog(), "close");
            QTRY_VERIFY(!isOpen());
        };
        click(app, tab);
        QTRY_VERIFY(isOpen());
        QVERIFY(!tab->property("open").toBool());
        QCOMPARE(app.sidePanels.tab(), QString()); // no panel opened
        close();
        tab->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(tab->hasActiveFocus());
        app.key(Qt::Key_Return);
        QTRY_VERIFY(isOpen());
        close();
        tab->forceActiveFocus(Qt::TabFocusReason);
        app.key(Qt::Key_Space);
        QTRY_VERIFY(isOpen());
        close();
        // The menu command opens the same dialog.
        QVERIFY(app.commands.trigger("file.templates"));
        QTRY_VERIFY(isOpen());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The Settings action tab: the lower end of the strip past the hairline, running app.settings
    // (Ctrl+,); never open or checked; the tooltip shows the key.
    void theSettingsActionTabOpensSettingsAtTheLowerEnd() {
        App app(load(small));
        QVERIFY(app.open());
        QQuickItem *strip = named(app, "sideTabStrip");
        QQuickItem *tab = named(app, "sideTab-settings");
        QQuickItem *rule = named(app, "sideTabActionsRule");
        QVERIFY(strip && tab && rule);
        QVERIFY(tab->width() >= 36 && tab->height() >= 36);
        QVERIFY(tab->activeFocusOnTab());
        QVERIFY(tab->property("isAction").toBool());
        QVERIFY(!tab->property("open").toBool());
        // The rule is only there for the bottom group: past the top tabs, above Settings, which ends the strip.
        QVERIFY(rule->isVisible());
        const QQuickItem *templates = named(app, "sideTab-templates");
        const qreal ruleY = rule->mapToItem(strip, QPointF(0, 0)).y();
        const qreal tabY = tab->mapToItem(strip, QPointF(0, 0)).y();
        QVERIFY(ruleY >= templates->mapToItem(strip, QPointF(0, 0)).y() + templates->height());
        QVERIFY(tabY > ruleY);
        QVERIFY(tabY + tab->height() <= strip->height());
        QCOMPARE(tab->property("tip").toString(), QString("Settings (Ctrl+,)"));
        const auto panel = [&app]() { return app.window->findChild<QObject *>("settingsPanel"); };
        const auto isOpen = [&]() { return panel() && panel()->property("opened").toBool(); };
        click(app, tab);
        QTRY_VERIFY(isOpen());
        QVERIFY(!tab->property("open").toBool());
        QCOMPARE(app.sidePanels.tab(), QString());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theStripSurvivesANarrowWindowAndFocusModeHidesIt() {
        App app(load(small));
        app.sidePanels.setTab("nodes");
        app.sidePanels.setWidth(600);
        QVERIFY(app.open());
        app.window->resize(420, 500);
        QTRY_COMPARE(app.window->width(), 420);
        QQuickItem *strip = named(app, "sideTabStrip");
        QVERIFY(strip);
        QTRY_VERIFY(app.canvas->width() >= 160 - 1); // the canvas is never squeezed out by the panel
        QVERIFY(named(app, "sideTab-nodes")->height() >= 36);
        // The action tabs stay inside a short strip: Templates under Workflows, Settings at the bottom.
        app.window->resize(420, 300);
        QTRY_COMPARE(app.window->height(), 300);
        QQuickItem *templates = named(app, "sideTab-templates");
        QVERIFY(templates->height() >= 36);
        QTRY_VERIFY(templates->mapToItem(strip, QPointF(0, 0)).y() + templates->height() <= strip->height());
        QTRY_VERIFY(templates->mapToItem(strip, QPointF(0, 0)).y() >= named(app, "sideTab-workflows")->mapToItem(strip, QPointF(0, 0)).y() + 36);
        QQuickItem *settings = named(app, "sideTab-settings");
        QTRY_VERIFY(settings->mapToItem(strip, QPointF(0, 0)).y() + settings->height() <= strip->height());
        QTRY_VERIFY(settings->mapToItem(strip, QPointF(0, 0)).y() >= templates->mapToItem(strip, QPointF(0, 0)).y() + 36);
        app.commands.run("view.focus-mode");
        QTRY_VERIFY(!named(app, "sideTabStrip"));
        QTRY_COMPARE(app.canvas->x(), 0.0);
        app.commands.run("view.focus-mode");
        QTRY_VERIFY(named(app, "sideTabStrip"));
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


    // The list scrolls in the real window (the owner's report: neither the wheel nor the scroll bar did):
    // the wheel over a row, over a category, over a library group's trash can, the thumb dragged both
    // ways, and a press on the track.
    void theLibraryListScrollsByTheWheelAndTheScrollBar() {
        App app(load(small));
        QVERIFY(app.open());
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibraryList"));
        QQuickItem *list = named(app, "nodeLibraryList");
        app.nodeLibrary.setQuery("e"); // every matching category open: far more rows than fit
        QTRY_VERIFY2(list->property("contentHeight").toDouble() > list->height() * 2,
                     qPrintable(QStringLiteral("%1 rows, list %2px").arg(rowsOf(app).size()).arg(list->height())));
        QQuickItem *bar = item(list, "fugueScrollBar");
        QVERIFY(bar);
        QTRY_VERIFY(bar->isVisible());
        const auto y = [&] { return list->property("contentY").toDouble(); };
        QCOMPARE(y(), 0.0);
        // The wheel over a node row.
        QQuickItem *row = rowItem(list, firstRow(app, false));
        QVERIFY(row);
        wheelAt(app.window, center(row), -120);
        QTRY_VERIFY(y() > 0);
        const qreal afterRow = y();
        // Over a category row.
        QQuickItem *category = nullptr;
        for (int i = 0; i < rowsOf(app).size() && !category; ++i)
            if (QQuickItem *r = rowItem(list, i); r && rowsOf(app)[i].toMap().value("category").toBool() && r->y() - y() >= 0)
                category = r;
        QVERIFY(category);
        wheelAt(app.window, center(category) + QPoint(0, int(y() - category->y() > 0 ? 0 : 0)), -120);
        QTRY_VERIFY(y() > afterRow);
        // Up again.
        const qreal before = y();
        wheelAt(app.window, center(list), 120);
        QTRY_VERIFY(y() < before);
        // A touchpad: pixel deltas only scroll too.
        const qreal pad = y();
        wheelAt(app.window, center(list), 0, -30);
        QTRY_VERIFY(y() > pad);
        // The thumb, dragged down and then up.
        list->setProperty("contentY", 0.0);
        QTRY_COMPARE(y(), 0.0);
        QQuickItem *thumb = bar->property("contentItem").value<QQuickItem *>();
        QVERIFY(thumb);
        const QPoint grab = thumb->mapToScene(QPointF(thumb->width() / 2, thumb->height() / 2)).toPoint();
        QTest::mousePress(app.window, Qt::LeftButton, {}, grab);
        for (int i = 1; i <= 10; ++i)
            QTest::mouseMove(app.window, grab + QPoint(0, i * 8));
        QTRY_VERIFY2(y() > 0, "dragging the thumb down must scroll");
        const qreal down = y();
        for (int i = 9; i >= 0; --i)
            QTest::mouseMove(app.window, grab + QPoint(0, i * 8));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, grab);
        QTRY_VERIFY2(y() < down, "dragging the thumb back up must scroll back");
        // A press on the track below the thumb pages.
        list->setProperty("contentY", 0.0);
        const QPoint below = bar->mapToScene(QPointF(bar->width() / 2, bar->height() - 4)).toPoint();
        QTest::mouseClick(app.window, Qt::LeftButton, {}, below);
        QTRY_VERIFY(y() > 0);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The owner's real session: the pointer rests on a row until its tooltip shows, and then the wheel
    // turns. The list must scroll with the tooltip up, by a mouse notch and by a scroll phase, over a
    // node row, a category row and the empty part of the panel.
    void theWheelScrollsWhileARowTooltipShows() {
        App app(load(small));
        QVERIFY(app.open());
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibraryList"));
        QQuickItem *list = named(app, "nodeLibraryList");
        app.nodeLibrary.setQuery("e");
        QTRY_VERIFY(list->property("contentHeight").toDouble() > list->height() * 2);
        const auto y = [&] { return list->property("contentY").toDouble(); };
        const auto tooltipShowing = [&] {
            for (QObject *o : app.engine.findChildren<QObject *>())
                if (QByteArray(o->metaObject()->className()).contains("ToolTip") && o->property("contentItem").isValid() && o->property("visible").toBool())
                    return true;
            return false;
        };
        struct Case { const char *name; bool category; int pixel; };
        for (const Case c : {Case{"node row, mouse", false, 0}, Case{"category row, mouse", true, 0},
                             Case{"node row, scroll phase", false, -30}}) {
            list->setProperty("contentY", 200.0);
            QTRY_COMPARE(y(), 200.0);
            QQuickItem *row = nullptr;
            for (int i = 0; i < rowsOf(app).size() && !row; ++i)
                if (QQuickItem *r = rowItem(list, i); r && rowsOf(app)[i].toMap().value("category").toBool() == c.category
                                                      && !rowsOf(app)[i].toMap().value("description").toString().isEmpty() == !c.category
                                                      && r->y() - y() >= 0)
                    row = r;
            QVERIFY2(row, c.name);
            QTest::mouseMove(app.window, center(row) + QPoint(-20, 0));
            QTest::mouseMove(app.window, center(row));
            if (!c.category)
                QTRY_VERIFY_WITH_TIMEOUT(tooltipShowing(), 3000);
            const qreal before = y();
            const QPoint pointer = center(row);
            wheelAt(app.window, pointer, -120, c.pixel);
            QTRY_VERIFY2(y() > before, c.name);
            const qreal mid = y();
            // The notch hides the tooltip of the row that moved away, at once and until the pointer moves.
            QVERIFY2(!tooltipShowing(), c.name);
            wheelAt(app.window, pointer, 120, c.pixel ? 30 : 0);
            QTRY_VERIFY2(y() < mid, c.name);
            // The wheel as the owner's Wayland session sends it: a TouchPad device, no phase, angle and pixels.
            const qreal low = y();
            scroll_checks::waylandWheelAt(app.window, pointer, -1);
            QTRY_VERIFY2(y() > low, qPrintable(QStringLiteral("%1, Wayland wheel").arg(c.name)));
        }
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The shared tooltip (canvas/ToolTipStyle.qml): a node's long description is wrapped at the
    // maximum width, inside the window, shown after the delay and beside the row, in the library and
    // on another surface (a side tab), with a very long text too.
    void aRowTooltipIsWrappedAndInsideTheWindow() {
        App app(load(small));
        QVERIFY(app.open());
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibraryList"));
        QQuickItem *list = named(app, "nodeLibraryList");
        app.nodeLibrary.setQuery("sampler");
        QTRY_VERIFY(!rowsOf(app).isEmpty());
        const double maxWidth = 80 * app.theme.look()->property("unit").toDouble();
        QVERIFY(maxWidth > 100);
        const auto tooltip = [&]() -> QObject * {
            for (QObject *o : app.engine.findChildren<QObject *>())
                if (QByteArray(o->metaObject()->className()).contains("ToolTip") && o->property("contentItem").isValid() && o->property("visible").toBool())
                    return o;
            return nullptr;
        };
        const auto inside = [&](QObject *tip, QQuickItem *parent) {
            const QQuickItem *content = tip->property("contentItem").value<QQuickItem *>();
            const QPointF at = parent->mapToScene(QPointF(tip->property("x").toDouble(), tip->property("y").toDouble()));
            return QRectF(at, QSizeF(tip->property("width").toDouble(), tip->property("height").toDouble()))
                       .intersected(QRectF(QPointF(0, 0), QSizeF(app.window->size()))) ==
                   QRectF(at, QSizeF(tip->property("width").toDouble(), tip->property("height").toDouble())) && content;
        };
        // A row with a description, hovered.
        QQuickItem *row = nullptr;
        for (int i = 0; i < rowsOf(app).size() && !row; ++i)
            if (!rowsOf(app)[i].toMap().value("category").toBool() && rowsOf(app)[i].toMap().value("description").toString().size() > 80)
                row = rowItem(list, i);
        QVERIFY2(row, "a node with a long description");
        QTest::mouseMove(app.window, center(row) + QPoint(-10, 0));
        QTest::mouseMove(app.window, center(row));
        QTest::qWait(300);
        QVERIFY2(!tooltip(), "not before the delay");
        QTRY_VERIFY_WITH_TIMEOUT(tooltip(), 3000);
        QObject *tip = tooltip();
        QVERIFY2(tip->property("width").toDouble() <= maxWidth + 0.5,
                 qPrintable(QStringLiteral("tooltip %1px wide, maximum %2").arg(tip->property("width").toDouble()).arg(maxWidth)));
        QVERIFY(tip->property("height").toDouble() > 40); // wrapped to more than one line
        QVERIFY(inside(tip, row));
        // It is below the row, not over it.
        QVERIFY(tip->property("y").toDouble() >= row->height());
        if (const QByteArray dir = qgetenv("FUGUE_SHOT_DIR"); !dir.isEmpty()) {
            QTest::qWait(600); // the fade in
            test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/library-tooltip.png");
        }
        // Another surface, with a text far wider than the window.
        QQuickItem *tab = named(app, "sideTab-nodes");
        QVERIFY(tab);
        tip->setProperty("text", QString("word ").repeated(600));
        QMetaObject::invokeMethod(tip, "close");
        tip->setProperty("parent", QVariant::fromValue(tab));
        QMetaObject::invokeMethod(tip, "open");
        QTRY_VERIFY(tooltip());
        QVERIFY2(tip->property("width").toDouble() <= maxWidth + 0.5, "a very long text stays at the maximum");
        QVERIFY(tip->property("width").toDouble() <= app.window->width());
        QVERIFY(inside(tip, tab));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // With the bar showing, no row's text lies under it and nothing sits under the panel's grip, on
    // either dock; and a library group's trash can (library row) is clear of the bar too.
    void theLibraryKeepsItsRowsClearOfTheScrollBarOnBothDocks() {
        for (const char *dock : {"left", "right"}) {
            App app(load(small));
            QVERIFY(app.open());
            app.sidePanels.setDock(QString::fromLatin1(dock));
            app.commands.run("panel.nodes");
            QTRY_VERIFY(named(app, "nodeLibraryList"));
            QQuickItem *list = named(app, "nodeLibraryList");
            app.nodeLibrary.setQuery("e");
            QTRY_VERIFY(scroll_checks::barOf(list) && scroll_checks::barOf(list)->isVisible());
            QQuickItem *bar = scroll_checks::barOf(list);
            const QString under = scroll_checks::contentUnderBar(list, bar);
            QVERIFY2(under.isEmpty(), qPrintable(QString::fromLatin1(dock) + ": " + under));
            QQuickItem *grip = named(app, "sidePanelGrip");
            QVERIFY(grip);
            const QRectF gripRect(grip->mapToScene(QPointF(0, 0)), grip->size());
            const QRectF barRect(bar->mapToScene(QPointF(0, 0)), bar->size());
            QVERIFY2(!gripRect.intersects(barRect), dock);
            const QString failed = scroll_checks::scrollsByWheelAndThumb(app.window, list);
            QVERIFY2(failed.isEmpty(), qPrintable(QString::fromLatin1(dock) + ": " + failed));
        }
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
        QTRY_COMPARE(app.canvas->x(), stripWidth(app) + width + 60);
        // The keys do it too, and the width has bounds.
        grip->forceActiveFocus();
        app.key(Qt::Key_Left);
        QTRY_COMPARE(app.sidePanels.width(), width + 60 - 16);
        app.sidePanels.setWidth(10);
        QTRY_COMPARE(named(app, "sidePanel")->width(), qreal(SidePanelsMin()));
        // To the other side: the canvas is at the left edge, the panel at the right.
        click(app, named(app, "sidePanelDock"));
        QTRY_COMPARE(app.sidePanels.dock(), QString("right"));
        QTRY_COMPARE(app.canvas->x(), stripWidth(app));
        QCOMPARE(app.canvas->width(), app.window->width() - stripWidth(app) - qreal(app.sidePanels.width()));
        QCOMPARE(named(app, "sidePanel")->x(), app.window->width() - qreal(app.sidePanels.width()));
        // Dragging the grip to the left widens it there.
        const QPoint right = center(named(app, "sidePanelGrip"));
        const int wide = app.sidePanels.width();
        app.drag(Qt::LeftButton, right, right - QPoint(30, 0));
        QTRY_COMPARE(app.sidePanels.width(), wide + 30);
        // The close button closes it.
        click(app, named(app, "sidePanelClose"));
        QTRY_VERIFY(!named(app, "sidePanel"));
        QTRY_COMPARE(app.canvas->width(), app.window->width() - stripWidth(app));
    }

    // A workflow with one group saved to the library as local/group (the library is the test's own).
    static bool saveGroup(App &app) {
        app.model.select({"prompt"});
        const QString instance = app.model.groups()->makeGroup();
        app.model.select({instance});
        return app.model.groups()->saveGroupToLibrary();
    }

    QQuickItem *libraryRow(App &app) {
        QQuickItem *list = named(app, "nodeLibraryList");
        QQuickItem *row = nullptr;
        [&] { QTRY_VERIFY((row = rowOfKey(app, list, "local/group"))); }();
        return row;
    }

    // #156: a trash can ends the row of a group of the library and no other row; it asks first.
    void onlyALibraryGroupsRowHasATrashCan() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY2(saveGroup(app), qPrintable(app.model.groups()->problem()));
        QTRY_VERIFY(app.registry.nodeType("local/group"));
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibraryList"));
        app.nodeLibrary.setQuery("o"); // every category on the way to a match is open; many rows
        QQuickItem *list = named(app, "nodeLibraryList");
        QQuickItem *mine = libraryRow(app);
        QVERIFY(item(mine, "nodeLibraryTrash"));
        int nodes = 0;
        for (int i = 0; i < rowsOf(app).size(); ++i) {
            const QVariantMap row = rowsOf(app)[i].toMap();
            if (row.value("category").toBool())
                continue;
            ++nodes;
            QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, i), Q_ARG(int, 0));
            QQuickItem *shown = rowItem(list, i);
            QVERIFY(shown);
            QCOMPARE(item(shown, "nodeLibraryTrash") != nullptr, row.value("key").toString() == "local/group");
        }
        QVERIFY(nodes > 1); // the built-in group nodes show none
        // The can is a 28px square at the row's far end, clear of the scroll bar.
        QQuickItem *trash = item(mine, "nodeLibraryTrash");
        QVERIFY(trash->width() >= 28 && trash->height() >= 28);
        QQuickItem *bar = item(list, "fugueScrollBar");
        QVERIFY(bar && bar->isVisible()); // so many rows match "o" that the list scrolls
        QVERIFY(trash->mapToItem(list, QPointF(trash->width(), 0)).x() <= list->width() - bar->width());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theTrashCanAsksAndCancelKeepsTheGroup() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY2(saveGroup(app), qPrintable(app.model.groups()->problem()));
        QTRY_VERIFY(app.registry.nodeType("local/group"));
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibraryList"));
        app.nodeLibrary.setQuery("group");
        const int nodes = app.model.document().graph.nodes.size();
        QQuickItem *trash = item(libraryRow(app), "nodeLibraryTrash");
        QVERIFY(trash);
        click(app, trash);
        QObject *ask = nullptr;
        QTRY_VERIFY((ask = app.window->findChild<QObject *>("deleteGroupQuestion")));
        QTRY_VERIFY(ask->property("visible").toBool());
        // It names the group and says what happens.
        QQuickItem *title = item(app.window->contentItem(), "deleteGroupTitle");
        QVERIFY(title);
        QVERIFY2(title->property("text").toString().contains("Group"), qPrintable(title->property("text").toString()));
        QVERIFY(item(app.window->contentItem(), "deleteGroupDetail")->property("text").toString().contains("cannot be undone", Qt::CaseInsensitive));
        // The click did not add a node, and nothing is deleted yet; Cancel has the focus, Cancel
        // is before Delete.
        QCOMPARE(app.model.document().graph.nodes.size(), nodes);
        QQuickItem *cancel = item(app.window->contentItem(), "deleteGroupCancel");
        QQuickItem *confirm = item(app.window->contentItem(), "deleteGroupConfirm");
        QVERIFY(cancel && confirm);
        QTRY_VERIFY(cancel->hasActiveFocus());
        QVERIFY(cancel->mapToScene(QPointF(0, 0)).x() < confirm->mapToScene(QPointF(0, 0)).x());
        app.key(Qt::Key_Return); // Enter is Cancel
        QTRY_VERIFY(!ask->property("visible").toBool());
        QVERIFY(QFileInfo(fugue::libraryDir() + "/group/node.json").isFile());
        QVERIFY(app.registry.nodeType("local/group"));
        // Escape cancels too.
        click(app, item(libraryRow(app), "nodeLibraryTrash"));
        QTRY_VERIFY(ask->property("visible").toBool());
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!ask->property("visible").toBool());
        QVERIFY(QFileInfo(fugue::libraryDir() + "/group/node.json").isFile());
        QCOMPARE(app.model.document().graph.nodes.size(), nodes);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void deleteRemovesTheGroupAtOnceAndTheOpenWorkflowKeepsItsCopy() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY2(saveGroup(app), qPrintable(app.model.groups()->problem()));
        QTRY_VERIFY(app.registry.nodeType("local/group"));
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibraryList"));
        app.nodeLibrary.setQuery("group");
        const fugue::Document before = app.model.document();
        QVERIFY2(before.groups.size() == 1, qPrintable(QString::number(before.groups.size())));
        const QString embedded = before.groups.firstKey();
        click(app, item(libraryRow(app), "nodeLibraryTrash"));
        QObject *ask = nullptr;
        QTRY_VERIFY((ask = app.window->findChild<QObject *>("deleteGroupQuestion")) && ask->property("visible").toBool());
        QQuickItem *confirm = item(app.window->contentItem(), "deleteGroupConfirm");
        QTest::qWait(100);
        click(app, confirm);
        QTRY_VERIFY(!ask->property("visible").toBool());
        // Gone from disk, the registry, the panel and node search, with a notice.
        QVERIFY(!QFileInfo(fugue::libraryDir() + "/group").exists());
        QTRY_VERIFY(!app.registry.nodeType("local/group"));
        QTRY_VERIFY(!rowOfKey(app, named(app, "nodeLibraryList"), "local/group"));
        for (const QVariant &row : rowsOf(app))
            QVERIFY(row.toMap().value("key").toString() != "local/group");
        QVERIFY2(app.model.groups()->notice().contains("Deleted"), qPrintable(app.model.groups()->notice()));
        QCOMPARE(app.model.groups()->problem(), QString());
        app.model.search()->openAt(0, 0);
        app.model.search()->setQuery("group");
        // Node search offers the open workflow's own definition of the group (4.4: it stands for its id), so a
        // row of local/group is there only because the workflow embeds it; the library's copy is gone.
        for (int i = 0; i < app.model.search()->results()->rowCount(); ++i) {
            const QString id = app.model.search()->results()->data(app.model.search()->results()->index(i), fugue::SearchResults::TypeId).toString();
            QVERIFY(id != "local/group" || app.model.document().group(id));
        }
        app.model.search()->close();
        // The open workflow keeps the definition it embeds, and still compiles to the same thing.
        QVERIFY(app.model.document().group(embedded));
        const fugue::Result<fugue::Document> again = fugue::Document::load(QJsonDocument(app.model.document().toJson()).toJson());
        QVERIFY(again.ok());
        QVERIFY(again->group(embedded));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The can is reached by Tab-focus and Enter, and a drag that starts on it takes nothing out.
    void theTrashCanIsKeyboardReachableAndNotADragHandle() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY2(saveGroup(app), qPrintable(app.model.groups()->problem()));
        QTRY_VERIFY(app.registry.nodeType("local/group"));
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibraryList"));
        app.nodeLibrary.setQuery("group");
        QQuickItem *trash = item(libraryRow(app), "nodeLibraryTrash");
        QVERIFY(trash->activeFocusOnTab());
        const int nodes = app.model.document().graph.nodes.size();
        // A drag from the can onto the canvas adds nothing, and asks nothing.
        const QPoint from = center(trash);
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(app.window, from + QPoint(i * 60, 0));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, from + QPoint(480, 0));
        settle();
        QCOMPARE(app.model.document().graph.nodes.size(), nodes);
        QObject *ask = app.window->findChild<QObject *>("deleteGroupQuestion");
        QVERIFY(!ask || !ask->property("visible").toBool());
        // Focus and Enter ask; Delete on the row does too.
        trash = item(libraryRow(app), "nodeLibraryTrash");
        trash->forceActiveFocus();
        app.key(Qt::Key_Return);
        QTRY_VERIFY((ask = app.window->findChild<QObject *>("deleteGroupQuestion")) && ask->property("visible").toBool());
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!ask->property("visible").toBool());
        QQuickItem *list = named(app, "nodeLibraryList");
        const QVariantList rows = rowsOf(app);
        for (int i = 0; i < rows.size(); ++i)
            if (rows[i].toMap().value("key").toString() == "local/group")
                list->setProperty("currentIndex", i);
        list->forceActiveFocus();
        app.key(Qt::Key_Delete);
        QTRY_VERIFY(ask->property("visible").toBool());
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!ask->property("visible").toBool());
        QVERIFY(QFileInfo(fugue::libraryDir() + "/group/node.json").isFile());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Not a check: with FUGUE_SHOT_DIR set, saves the window with the tab strip, dark and light,
    // closed and with the node library open (docs/recording-screenshots.md).
    void savesTheStripShots_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("name");
        QTest::newRow("dark") << QString() << "side-tabs";
        QTest::newRow("light") << "catppuccin-latte" << "side-tabs-light";
    }
    void savesTheStripShots() {
        QFETCH(QString, theme);
        QFETCH(QString, name);
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the pictures");
        App app(load(small), {}, Look{theme, 8});
        QVERIFY(app.open());
        app.window->resize(1000, 640);
        QTest::qWait(300);
        test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/" + name + "-closed.png");
        click(app, named(app, "sideTab-nodes"));
        QTRY_VERIFY(named(app, "nodeLibrary"));
        QTest::mouseMove(app.window, QPoint(700, 500));
        QTest::qWait(300);
        test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/" + name + "-open.png");
        // The Templates tab pressed: the dialog is up over the strip, whose action tab is not marked.
        click(app, named(app, "sideTab-templates"));
        QTRY_VERIFY(app.window->findChild<QObject *>("templatesDialog")
                    && app.window->findChild<QObject *>("templatesDialog")->property("opened").toBool());
        QTest::qWait(300);
        test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/" + name + "-templates.png");
    }

    // Not a check: with FUGUE_SHOT_DIR set, saves the panel with a library group and the question.
    void savesTheLibraryDeleteShots() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the pictures");
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY2(saveGroup(app), qPrintable(app.model.groups()->problem()));
        QTRY_VERIFY(app.registry.nodeType("local/group"));
        app.commands.run("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibraryList"));
        app.nodeLibrary.setQuery("group");
        QTest::qWait(300);
        test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/library-delete-panel.png");
        click(app, item(libraryRow(app), "nodeLibraryTrash"));
        QObject *ask = nullptr;
        QTRY_VERIFY((ask = app.window->findChild<QObject *>("deleteGroupQuestion")) && ask->property("visible").toBool());
        QTest::qWait(400);
        test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/library-delete-question.png");
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
            QCOMPARE(named(app, "sidePanel")->x(), stripWidth(app));
            QCOMPARE(app.canvas->x(), stripWidth(app) + panel);
            QCOMPARE(app.canvas->width(), app.window->width() - stripWidth(app) - panel);
            app.sidePanels.setDock("right");
            QTRY_COMPARE(app.canvas->x(), stripWidth(app));
            QCOMPARE(app.canvas->width(), app.window->width() - stripWidth(app) - panel);
            QCOMPARE(named(app, "sidePanel")->x(), app.window->width() - panel);
        }
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
    }
};

QTEST_MAIN(TestSidePanelsWindow)
#include "test_side_panels_window.moc"
