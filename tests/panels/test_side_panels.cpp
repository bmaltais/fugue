// What the side panels remember (#56): the open one, its side and its width, kept between runs,
// and the two commands that toggle them.

#include <QTemporaryDir>
#include <QtTest>

#include "commands/command_registry.h"
#include "menu/menu_commands.h"
#include "panels/chrome_visibility.h"
#include "panels/side_panels.h"

using namespace fugue;

class TestSidePanels : public QObject {
    Q_OBJECT

    static bool checked(const CommandRegistry &commands, const QString &id) {
        return commands.entry(id).value("checked").toBool();
    }

private slots:
    void startsClosedOnTheLeft() {
        SidePanels panels({});
        QCOMPARE(panels.tab(), QString());
        QCOMPARE(panels.dock(), QString("left"));
        QCOMPARE(panels.width(), int(SidePanels::kDefaultWidth));
    }
    void oneIsOpenAtATimeAndTheSameKeyCloses() {
        SidePanels panels({});
        QSignalSpy focus(&panels, &SidePanels::focusRequested);
        panels.toggle("nodes");
        QCOMPARE(panels.tab(), QString("nodes"));
        panels.toggle("workflows");
        QCOMPARE(panels.tab(), QString("workflows"));
        panels.toggle("workflows");
        QCOMPARE(panels.tab(), QString());
        QCOMPARE(focus.count(), 2); // closing asks for nothing
    }
    void anUnknownTabIsNone() {
        SidePanels panels({});
        panels.setTab("nodes");
        panels.setTab("models");
        QCOMPARE(panels.tab(), QString());
    }
    void widthStaysInsideItsBounds() {
        SidePanels panels({});
        panels.setWidth(10);
        QCOMPARE(panels.width(), int(SidePanels::kMinWidth));
        panels.setWidth(5000);
        QCOMPARE(panels.width(), int(SidePanels::kMaxWidth));
    }
    void itIsKeptBetweenRuns() {
        QTemporaryDir dir;
        {
            ChromeVisibility chrome(dir.filePath("chrome.json"));
            SidePanels panels(dir.filePath("side-panels.json"), &chrome);
            panels.setTab("workflows");
            panels.setDock("right");
            panels.setWidth(333);
        }
        ChromeVisibility chrome(dir.filePath("chrome.json"));
        SidePanels again(dir.filePath("side-panels.json"), &chrome);
        QCOMPARE(again.tab(), QString("workflows"));
        QCOMPARE(again.dock(), QString("right"));
        QCOMPARE(again.width(), 333);
    }
    void aTabKeptByAnOlderFileIsReadOnceAndThenTheRegistryDecides() {
        QTemporaryDir dir;
        const QString path = dir.filePath("side-panels.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"tab\": \"nodes\", \"dock\": \"right\", \"width\": 300}");
        file.close();
        {
            ChromeVisibility chrome(dir.filePath("chrome.json"));
            SidePanels panels(path, &chrome);
            QCOMPARE(panels.tab(), QString("nodes"));
            QCOMPARE(panels.dock(), QString("right"));
            panels.setTab(""); // closed: the registry now has its own answer
        }
        ChromeVisibility chrome(dir.filePath("chrome.json"));
        SidePanels again(path, &chrome);
        QCOMPARE(again.tab(), QString());
    }
    void focusModeHidesThePanelAndBringsItBack() {
        ChromeVisibility chrome({});
        SidePanels panels({}, &chrome);
        QSignalSpy changed(&panels, &SidePanels::tabChanged);
        panels.setTab("nodes");
        chrome.setFocusMode(true);
        QCOMPARE(panels.tab(), QString());
        chrome.setFocusMode(false);
        QCOMPARE(panels.tab(), QString("nodes"));
        QCOMPARE(changed.count(), 3);
        panels.toggle("workflows"); // switching sends one change
        QCOMPARE(changed.count(), 4);
    }
    // #192: an action row is a tab, not a panel: it has no chrome id, toggle and setTab ignore it,
    // and it is never open.
    void anActionRowIsATabButNeverAPanel() {
        ChromeVisibility chrome({});
        SidePanels panels({}, &chrome);
        const QVariantList list = panels.panels();
        QCOMPARE(list.size(), 3);
        int panelRows = 0, actionRows = 0;
        for (const QVariant &row : list) {
            const QVariantMap map = row.toMap();
            (map.value("kind").toString() == "action" ? actionRows : panelRows)++;
        }
        QCOMPARE(panelRows, 2);
        QCOMPARE(actionRows, 1);
        QCOMPARE(list.last().toMap().value("tab").toString(), QString("templates"));
        panels.toggle("templates");
        QCOMPARE(panels.tab(), QString());
        panels.setTab("templates");
        QCOMPARE(panels.tab(), QString());
        QVERIFY(!panels.panels().last().toMap().value("open").toBool());
        QVERIFY(!chrome.isVisible("templates"));
    }
    void aDamagedFileGivesTheDefaults() {
        QTemporaryDir dir;
        QFile file(dir.filePath("side-panels.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"tab\": 3, \"dock\": \"up\", \"width\": 99999}");
        file.close();
        SidePanels panels(file.fileName());
        QCOMPARE(panels.tab(), QString());
        QCOMPARE(panels.dock(), QString("left"));
        QCOMPARE(panels.width(), int(SidePanels::kMaxWidth));
    }
    void thereIsATabPerRegisteredPanelWithItsKeyAndItsOpenMark() {
        CommandRegistry commands;
        registerDefaultCommands(commands);
        registerMenuCommands(commands);
        SidePanels panels({});
        panels.registerCommands(commands);
        QVariantList list = panels.panels();
        QCOMPARE(list.size(), 3); // two panels and the Templates action (#192)
        QCOMPARE(list[0].toMap().value("tab").toString(), QString("nodes"));
        QCOMPARE(list[0].toMap().value("keys").toStringList(), QStringList{"N"});
        QCOMPARE(list[1].toMap().value("tab").toString(), QString("workflows"));
        QCOMPARE(list[1].toMap().value("keys").toStringList(), QStringList{"W"});
        QCOMPARE(list[2].toMap().value("tab").toString(), QString("templates"));
        QCOMPARE(list[2].toMap().value("kind").toString(), QString("action"));
        QCOMPARE(list[2].toMap().value("command").toString(), QString("file.templates"));
        QCOMPARE(list[2].toMap().value("glyph").toString(), QString("layout-template"));
        for (const QVariant &row : list) {
            QVERIFY(!row.toMap().value("name").toString().isEmpty());
            QVERIFY(!row.toMap().value("glyph").toString().isEmpty());
            QVERIFY(!row.toMap().value("open").toBool());
            // An action's command is the window's (file.templates asks it for the dialog): no handler here.
            if (row.toMap().value("kind").toString() == "panel")
                QVERIFY(commands.entry(row.toMap().value("command").toString()).value("available").toBool());
        }
        QSignalSpy changed(&panels, &SidePanels::tabChanged);
        commands.run("panel.workflows");
        list = panels.panels();
        QVERIFY(!list[0].toMap().value("open").toBool());
        QVERIFY(list[1].toMap().value("open").toBool());
        QCOMPARE(changed.count(), 1); // the strip's `panels` binding follows tabChanged
        QVERIFY(!list[2].toMap().value("open").toBool());
        QVERIFY(!commands.entry("file.templates").value("checked").toBool()); // an action is never checked
    }
    void theStripIsShownButFocusModeHidesIt() {
        ChromeVisibility chrome({});
        SidePanels panels({}, &chrome);
        QVERIFY(chrome.isVisible("side-tabs"));
        chrome.setFocusMode(true);
        QVERIFY(!chrome.isVisible("side-tabs"));
    }
    void theCommandsToggleAndAreChecked() {
        CommandRegistry commands;
        registerDefaultCommands(commands);
        registerMenuCommands(commands);
        SidePanels panels({});
        panels.registerCommands(commands);
        QVERIFY(!checked(commands, "panel.nodes"));
        commands.run("panel.nodes");
        QCOMPARE(panels.tab(), QString("nodes"));
        QVERIFY(checked(commands, "panel.nodes"));
        QVERIFY(!checked(commands, "panel.workflows"));
        commands.run("panel.workflows");
        QVERIFY(checked(commands, "panel.workflows"));
        QVERIFY(!checked(commands, "panel.nodes"));
    }
};

QTEST_MAIN(TestSidePanels)
#include "test_side_panels.moc"
