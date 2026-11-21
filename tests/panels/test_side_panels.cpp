// What the side panels remember (#56): the open one, its side and its width, kept between runs,
// and the two commands that toggle them.

#include <QTemporaryDir>
#include <QtTest>

#include "commands/command_registry.h"
#include "menu/menu_commands.h"
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
            SidePanels panels(dir.filePath("side-panels.json"));
            panels.setTab("workflows");
            panels.setDock("right");
            panels.setWidth(333);
        }
        SidePanels again(dir.filePath("side-panels.json"));
        QCOMPARE(again.tab(), QString("workflows"));
        QCOMPARE(again.dock(), QString("right"));
        QCOMPARE(again.width(), 333);
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
