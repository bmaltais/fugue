// The window's layout of areas (#217 slice 1, workspace/workspace.h), alone, with no window: split, close and the
// view an area shows, the Graph that always keeps one area, the borders, the area commands, and the file
// (written a moment after a change, never for the default layout, removed when the layout is the default again,
// read at start, a damaged one kept as .bad with one line for the notice).

#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "commands/command_registry.h"
#include "workspace/workspace.h"

using namespace fugue;

namespace {

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

QString root(const Workspace &w) { return QJsonDocument(w.areaTree().toJson()).toJson(QJsonDocument::Compact); }

} // namespace

class TestWorkspace : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    QString file() const { return dir.filePath("workspaces.json"); }

private slots:
    void init() {
        QFile::remove(file());
        QFile::remove(file() + ".bad");
    }

    void theDefaultLayoutIsOneGraphAreaAndKeepsNoFile() {
        Workspace w(file());
        w.restore();
        QCOMPARE(w.count(), 1);
        QCOMPARE(w.graphArea(), 1);
        QCOMPARE(w.active(), 1);
        QCOMPARE(w.viewOf(1), QString("graph"));
        w.flush();
        QVERIFY(!QFile::exists(file()));
    }

    void aSplitShowsACanvasAndIsWrittenAMomentLater() {
        Workspace w(file());
        QSignalSpy changed(&w, &Workspace::changed);
        QCOMPARE(w.splitArea(1, "row"), 2);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(w.viewOf(2), QString("canvas")); // the Graph is one per window
        QCOMPARE(w.active(), 2);                  // the new area
        QVERIFY(!QFile::exists(file()));
        QTRY_VERIFY(QFile::exists(file()));
        QCOMPARE(AreaTree::load(readFile(file())).toJson(), w.areaTree().toJson());
        // A Canvas split gives a Canvas: it may be shown twice.
        QCOMPARE(w.splitArea(2, "column"), 3);
        QCOMPARE(w.viewOf(3), QString("canvas"));
        QCOMPARE(w.splitArea(3, "diagonal"), 0);
        QCOMPARE(w.splitArea(9, "row"), 0);
    }

    void aSplitThatWouldNotFitTheBodyIsRefused() {
        Workspace w;
        w.setBodySize(QSizeF(380, 400)); // Graph 160 + Canvas 240 = 400 wide
        QVERIFY(!w.canSplit(1, "row"));
        QVERIFY(w.canSplit(1, "column")); // 120 + 180 = 300 tall
        w.setBodySize(QSizeF(400, 400));
        QVERIFY(w.canSplit(1, "row"));
        w.setBodySize({});
        QVERIFY(w.canSplit(1, "row")); // no window: every split fits
    }

    // view.toggle-app in a split (#450): the active area shows the other of the App and the Graph, the two areas trading.
    void toggleAppTradesTheActiveAreasViewBetweenTheAppAndTheGraph() {
        Workspace w;
        QVERIFY(!w.toggleApp()); // one area: nothing to trade (the tab's mode is the window's)
        w.splitArea(1, "row");
        QCOMPARE(w.active(), 2);
        QCOMPARE(w.areaOf("app"), 0);
        // A Canvas area shows the App; again, the Graph comes (area 1 then shows the App).
        QVERIFY(w.toggleApp());
        QCOMPARE(w.viewOf(2), QString("app"));
        QVERIFY(w.toggleApp());
        QCOMPARE(w.viewOf(2), QString("graph"));
        QCOMPARE(w.viewOf(1), QString("app"));
        QCOMPARE(w.areaOf("app"), 1);
        // From the Graph's area the App comes from the area that shows it.
        QVERIFY(w.toggleApp());
        QCOMPARE(w.viewOf(2), QString("app"));
        QCOMPARE(w.viewOf(1), QString("graph"));
        // Asking for the App when an area shows it changes nothing; asking for the graph in the Graph's area neither.
        QVERIFY(!w.requestApp(true));
        w.setActive(1);
        QVERIFY(!w.requestApp(false));
        w.setActive(2);
        QVERIFY(w.requestApp(false));
        QCOMPARE(w.graphArea(), 2);
        // The Graph's area with no App anywhere gives it to the next area; the Canvas is what that area showed.
        QCOMPARE(w.areaOf("app"), 1);
        QVERIFY(w.showView(1, "canvas"));
        QVERIFY(w.requestApp(true));
        QCOMPARE(w.viewOf(1), QString("app"));
    }

    void theGraphKeepsAnAreaAndMovesWhereItIsChosen() {
        Workspace w;
        w.splitArea(1, "row");
        w.setAreaState(2, {{"canvasId", "c1"}});
        QVERIFY(!w.canShow(1, "canvas")); // its area holds the Graph
        QVERIFY(!w.showView(1, "canvas"));
        QVERIFY(w.canShow(2, "graph"));
        QVERIFY(!w.showView(2, "canvas")); // already
        QVERIFY(!w.showView(2, "storyboard"));
        const QVariantList choices = w.viewChoices(2);
        QCOMPARE(choices.size(), 3);
        QCOMPARE(choices.at(0).toMap().value("id").toString(), QString("graph"));
        QVERIFY(choices.at(0).toMap().value("moves").toBool());
        QVERIFY(choices.at(1).toMap().value("current").toBool());
        QVERIFY(!w.viewChoices(1).at(1).toMap().value("enabled").toBool());
        // Choosing Graph in area 2 trades: area 1 shows what area 2 did, its canvas too.
        QVERIFY(w.showView(2, "graph"));
        QCOMPARE(w.graphArea(), 2);
        QCOMPARE(w.viewOf(1), QString("canvas"));
        QCOMPARE(w.areaState(1).value("canvasId").toString(), QString("c1"));
        // Closing the Graph's area gives the Graph to the area that takes the place.
        QVERIFY(w.closeArea(2));
        QCOMPARE(w.count(), 1);
        QCOMPARE(w.viewOf(1), QString("graph"));
        QCOMPARE(w.active(), 1);
        // The canvas area 1 showed is still in its state, for when it shows a Canvas again.
        QCOMPARE(w.areaTree().area(1)->state.value("canvas").toObject().value("canvasId").toString(), QString("c1"));
        QVERIFY(!w.closeArea(1)); // the only area
    }

    void backToOneGraphAreaRemovesTheFile() {
        Workspace w(file());
        w.splitArea(1, "row");
        w.flush();
        QVERIFY(QFile::exists(file()));
        QVERIFY(w.closeArea(2));
        QVERIFY(!QFile::exists(file()));
        w.splitArea(1, "column");
        w.reset();
        QCOMPARE(w.count(), 1);
        QVERIFY(!QFile::exists(file()));
        w.flush();
        QVERIFY(!QFile::exists(file()));
    }

    void theBordersMoveAndShareEqually() {
        Workspace w;
        w.splitArea(1, "row");
        QVERIFY(w.resizeBorder({}, 0, 100, 1000));
        QVERIFY(qAbs(w.areaTree().root().children[0].size - 0.6) < 1e-9);
        QVERIFY(!w.resizeBorder({0}, 0, 100, 1000)); // a leaf has no border
        QVERIFY(w.equalizeBorder({}, 0));
        QVERIFY(qAbs(w.areaTree().root().children[0].size - 0.5) < 1e-9);
        QVERIFY(!w.equalizeBorder({}, 0));
    }

    void theAreaStateIsKeptOnlyWhenItChanges() {
        Workspace w;
        w.splitArea(1, "row");
        QSignalSpy changed(&w, &Workspace::changed);
        QVERIFY(w.setAreaState(2, {{"canvasId", "c1"}}));
        QVERIFY(!w.setAreaState(2, {{"canvasId", "c1"}}));
        QVERIFY(!w.setAreaState(9, {{"canvasId", "c1"}}));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(w.areaState(2).value("canvasId").toString(), QString("c1"));
    }

    void aKeptLayoutComesBackWithItsStates() {
        {
            Workspace w(file());
            w.splitArea(1, "row");
            w.setAreaState(2, {{"canvasId", "c1"}});
            w.resizeBorder({}, 0, -100, 1000);
        } // written at the end
        Workspace again(file());
        QSignalSpy messages(&again, &Workspace::message);
        again.restore();
        QCOMPARE(again.count(), 2);
        QCOMPARE(again.viewOf(2), QString("canvas"));
        QCOMPARE(again.areaState(2).value("canvasId").toString(), QString("c1"));
        QVERIFY(qAbs(again.areaTree().root().children[0].size - 0.4) < 1e-9);
        QCOMPARE(again.active(), 1); // the Graph's: the active area is not kept
        QCOMPARE(messages.count(), 0);
    }

    void aDamagedFileGivesTheDefaultAndIsKeptAsBad_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("not json") << QByteArray("{\"format\": 1, \"root\":");
        QTest::newRow("another format") << QByteArray(R"({"format": 2, "root": {"type": "leaf", "area": 1, "view": "graph"}})");
        QTest::newRow("a bad tree") << QByteArray(R"({"format": 1, "root": {"type": "leaf", "area": -1, "view": "graph"}})");
        QTest::newRow("too large") << QByteArray(Workspace::kMaxFileSize + 1, ' ');
    }
    void aDamagedFileGivesTheDefaultAndIsKeptAsBad() {
        QFETCH(QByteArray, bytes);
        writeFile(file(), bytes);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^workspace: .*workspaces.json"));
        Workspace w(file());
        QSignalSpy messages(&w, &Workspace::message);
        w.restore();
        QCOMPARE(w.count(), 1);
        QCOMPARE(w.viewOf(1), QString("graph"));
        QCOMPARE(messages.count(), 1);
        QVERIFY(messages.first().first().toString().contains("workspaces.json.bad"));
        QVERIFY(!QFile::exists(file()));
        QCOMPARE(readFile(file() + ".bad"), bytes);
    }

    void aLayoutWithoutTheGraphGetsItInItsFirstArea() {
        writeFile(file(), R"({"format": 1, "root": {"type": "branch", "axis": "row", "children": [
            {"size": 0.5, "node": {"type": "leaf", "area": 4, "view": "canvas", "state": {"canvas": {"canvasId": "c1"}}}},
            {"size": 0.5, "node": {"type": "leaf", "area": 7, "view": "storyboard"}}]}})");
        Workspace w(file());
        w.restore();
        QCOMPARE(w.viewOf(4), QString("graph"));
        QCOMPARE(w.viewOf(7), QString("storyboard")); // a newer build's view is kept
        QCOMPARE(w.areaTree().area(4)->state.value("canvas").toObject().value("canvasId").toString(), QString("c1"));
        QCOMPARE(w.active(), 4);
        // A second Graph shows a Canvas.
        writeFile(file(), R"({"format": 1, "root": {"type": "branch", "axis": "column", "children": [
            {"size": 0.5, "node": {"type": "leaf", "area": 1, "view": "graph"}},
            {"size": 0.5, "node": {"type": "leaf", "area": 2, "view": "graph"}}]}})");
        Workspace twice(file());
        twice.restore();
        QCOMPARE(twice.viewOf(1), QString("graph"));
        QCOMPARE(twice.viewOf(2), QString("canvas"));
    }

    void theCommandsActOnTheActiveArea() {
        CommandRegistry commands;
        Workspace w;
        w.registerCommands(commands);
        for (const char *id : {"area.split-row", "area.split-column", "area.close", "area.view-graph", "area.view-canvas",
                               "view.reset-layout"})
            QVERIFY2(commands.contains(id), id);
        QCOMPARE(commands.keys("area.split-row"), QStringList({"Ctrl+\\"}));
        QCOMPARE(commands.keys("area.split-column"), QStringList({"Ctrl+Alt+\\"}));
        QCOMPARE(commands.keys("area.close"), QStringList({"Ctrl+Alt+W"}));
        QCOMPARE(commands.keys("area.view-graph"), QStringList({"Ctrl+Alt+G"}));
        QCOMPARE(commands.keys("area.view-canvas"), QStringList({"Ctrl+Alt+C"}));
        QVERIFY(commands.keys("view.reset-layout").isEmpty());
        // One area: only the splits can run.
        QVERIFY(commands.isEnabled("area.split-row"));
        QVERIFY(!commands.isEnabled("area.close"));
        QVERIFY(!commands.isEnabled("area.view-canvas"));
        QVERIFY(!commands.isEnabled("view.reset-layout"));
        QVERIFY(commands.entry("area.view-graph").value("checked").toBool());
        QVERIFY(commands.triggerKey("Ctrl+\\"));
        QCOMPARE(w.count(), 2);
        QCOMPARE(w.active(), 2);
        QVERIFY(commands.entry("area.view-canvas").value("checked").toBool());
        QVERIFY(commands.trigger("area.view-graph")); // moves the Graph into the active area
        QCOMPARE(w.graphArea(), 2);
        QVERIFY(commands.trigger("area.split-column"));
        QCOMPARE(w.count(), 3);
        QCOMPARE(w.viewOf(3), QString("canvas"));
        w.setActive(9); // not an area: stays
        QCOMPARE(w.active(), 3);
        QVERIFY(commands.trigger("area.close"));
        QCOMPARE(w.count(), 2);
        QVERIFY(commands.trigger("view.reset-layout"));
        QCOMPARE(root(w), root(Workspace()));
    }
};

QTEST_GUILESS_MAIN(TestWorkspace)
#include "test_workspace.moc"
