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
