// What the run queue panel lists and does (spec 9.8, #60).
//
// Seam: RunQueue on temporary queue and run folders, as the CLI's `fugue run` leaves them. A
// Lock held in the test stands for the worker of a run that is going.

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/clock.h"
#include "engine/queue.h"
#include "engine/run_folder.h"
#include "run/run_queue.h"

using namespace fugue;

class TestRunQueue : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    RunQueuePaths paths() const {
        return {m_dir.filePath("runs"), m_dir.filePath("queue"), m_dir.filePath("engine.lock")};
    }
    Queue queue() const { return Queue(m_dir.filePath("queue")); }

    // A run `fugue run` queued: its folder and its entry.
    void queued(const QString &id, const QString &workflow, bool first = false) {
        const Result<RunFolder> folder = RunFolder::create(m_dir.filePath("runs"), id);
        QVERIFY(folder.ok());
        QVERIFY(folder->queued(workflow));
        QVERIFY(queue().enqueue(id, workflow, first));
    }
    void ended(const QString &id, RunStatus status, const QString &workflow, const QString &started = {}) {
        const Result<RunFolder> folder = RunFolder::create(m_dir.filePath("runs"), id);
        QVERIFY(folder.ok());
        QVERIFY(folder->queued(workflow));
        QJsonObject state = folder->state();
        if (!started.isEmpty())
            state.insert("started", started);
        folder->writeState(state);
        folder->finish(status, {}, QDateTime::fromString("2026-11-21T10:00:30.000Z", Qt::ISODateWithMs));
    }
    static QStringList runs(const QVariantList &rows) {
        QStringList list;
        for (const QVariant &row : rows)
            list << row.toMap().value("run").toString();
        return list;
    }

private slots:
    void init() { QDir(m_dir.path()).removeRecursively(); QDir().mkpath(m_dir.path()); }

    void aRunStartedFromTheCliAppearsAmongTheWaitingOnes() {
        RunQueue list(paths());
        QVERIFY(list.entries().isEmpty());
        QSignalSpy changed(&list, &RunQueue::changed);
        queued("20261121-100000-001-aaaa", "/w/portrait.fugue");
        QTRY_VERIFY(changed.count() > 0); // the watch on the queue folder
        QCOMPARE(list.count(), 1);
        const QVariantMap row = list.entries().first().toMap();
        QCOMPARE(row.value("status").toString(), QStringLiteral("waiting"));
        QCOMPARE(row.value("position").toInt(), 1);
        QCOMPARE(row.value("source").toString(), QStringLiteral("cli"));
        QVERIFY(!row.value("mine").toBool());
        QCOMPARE(row.value("name").toString(), QStringLiteral("portrait"));
        QVERIFY(row.value("since").toLongLong() > QDateTime::currentMSecsSinceEpoch() - 5000);
    }

    void theRunGoingComesFirstAndTheWaitingOnesFollowInQueueOrder() {
        Lock worker(paths().lockPath);
        QVERIFY(worker.tryAcquire()); // a worker holds the engine lock while its run goes
        queued("run-a", "/w/a.fugue");
        QVERIFY(queue().activate("run-a"));
        queued("run-b", "/w/b.fugue");
        queued("run-c", "");
        queued("run-d", "/w/d.fugue", true);
        RunQueue list(paths());
        QCOMPARE(runs(list.entries()), (QStringList{"run-a", "run-d", "run-b", "run-c"}));
        QCOMPARE(list.entries().at(0).toMap().value("status").toString(), QStringLiteral("running"));
        QCOMPARE(list.entries().at(0).toMap().value("position").toInt(), 0);
        QCOMPARE(list.entries().at(3).toMap().value("position").toInt(), 3);
        QCOMPARE(list.entries().at(3).toMap().value("name").toString(), QStringLiteral("Unsaved workflow"));
    }

    void aRunOfThisWindowIsMineAndAnotherProcessesIsNot() {
        const Result<RunFolder> folder = RunFolder::create(m_dir.filePath("runs"), "run-w");
        QVERIFY(folder.ok());
        QVERIFY(queue().enqueueForThisProcess("run-w", "/w/w.fugue", false));
        RunQueue list(paths());
        const QVariantMap row = list.entries().first().toMap();
        QCOMPARE(row.value("source").toString(), QStringLiteral("window"));
        QVERIFY(row.value("mine").toBool());
    }

    void movingToFrontReordersTheWaitingRunsAndRefusesTheOneGoing() {
        Lock worker(paths().lockPath);
        QVERIFY(worker.tryAcquire());
        queued("run-a", "/w/a.fugue");
        QVERIFY(queue().activate("run-a"));
        queued("run-b", "/w/b.fugue");
        queued("run-c", "/w/c.fugue");
        RunQueue list(paths());
        QVERIFY(list.moveToFront("run-c"));
        list.refresh();
        QCOMPARE(runs(list.entries()), (QStringList{"run-a", "run-c", "run-b"}));
        QVERIFY(!list.moveToFront("run-a"));
    }

    void cancellingAWaitingCliRunTakesItOutAndEndsItAsCancelled() {
        queued("run-b", "/w/b.fugue");
        RunQueue list(paths());
        list.cancel("run-b");
        list.refresh();
        QVERIFY(list.entries().isEmpty());
        const Result<RunFolder> folder = RunFolder::open(m_dir.filePath("runs"), "run-b");
        QCOMPARE(folder->status(), RunStatus::Cancelled);
    }

    void aRunWhoseWorkerIsGoneIsEndedAndLeavesTheList() {
        // An active entry with nobody holding the engine lock: its worker died.
        queued("run-a", "/w/a.fugue");
        QVERIFY(queue().activate("run-a"));
        RunQueue list(paths());
        list.refresh();
        QVERIFY(list.entries().isEmpty());
        QCOMPARE(RunFolder::open(m_dir.filePath("runs"), "run-a")->status(), RunStatus::Failed);
    }

    void aFinishedRowCarriesWhatTheRunMade() {
        QTemporaryDir place;
        QImage image(8, 8, QImage::Format_RGB32);
        const QString file = place.filePath("fox.png");
        QVERIFY(image.save(file));
        ended("20261121-110000-000-cccc", RunStatus::Ok, "/w/c.fugue");
        const RunFolder folder = RunFolder::open(m_dir.filePath("runs"), "20261121-110000-000-cccc").value();
        QJsonObject state = folder.state();
        state.insert("nodes", QJsonObject{{"save", QJsonObject{{"state", "done"}, {"outputs", QJsonObject{{"i", QJsonObject{{"type", "image"}, {"path", file}}}}}}}});
        QVERIFY(folder.writeState(state));
        ended("20261121-110001-000-cccc", RunStatus::Ok, "/w/c.fugue"); // made nothing
        RunQueue list(paths());
        list.setWatching(true);
        const QVariantMap made = list.history().at(1).toMap();
        QCOMPARE(made.value("lead").toString(), file);
        QCOMPARE(made.value("leadKind").toString(), QStringLiteral("image"));
        QCOMPARE(made.value("label").toString(), QStringLiteral("fox.png"));
        const QVariantMap nothing = list.history().at(0).toMap();
        QVERIFY(nothing.value("lead").toString().isEmpty());
        QVERIFY(nothing.value("label").toString().isEmpty());
    }

    void theHistoryIsTheLastEightEndedRunsNewestFirstAndOnlyWhileWatching() {
        for (int i = 1; i <= 10; ++i)
            ended(QStringLiteral("20261121-1000%1-000-aaaa").arg(i, 2, 10, QLatin1Char('0')),
                  i == 10 ? RunStatus::Failed : i == 9 ? RunStatus::Cancelled : RunStatus::Ok, "/w/h.fugue",
                  "2026-11-21T10:00:00.000Z");
        queued("20261121-100099-000-bbbb", "/w/q.fugue");
        RunQueue list(paths());
        QVERIFY(list.history().isEmpty());
        list.setWatching(true);
        QCOMPARE(list.history().size(), RunQueue::historyLimit);
        const QVariantMap newest = list.history().first().toMap();
        QCOMPARE(newest.value("run").toString(), QStringLiteral("20261121-100010-000-aaaa"));
        QCOMPARE(newest.value("status").toString(), QStringLiteral("failed"));
        QCOMPARE(newest.value("seconds").toDouble(), 30.0);
        QCOMPARE(newest.value("name").toString(), QStringLiteral("h"));
        QCOMPARE(list.history().at(1).toMap().value("status").toString(), QStringLiteral("cancelled"));
        QVERIFY(!runs(list.history()).contains("20261121-100099-000-bbbb")); // still waiting: not history
        list.setWatching(false);
        QVERIFY(list.history().isEmpty());
    }

    void theRunGoingShowsItsNodeAndProgressFromItsEventsWhileWatching() {
        Lock worker(paths().lockPath);
        QVERIFY(worker.tryAcquire());
        queued("run-a", "/w/a.fugue");
        QVERIFY(queue().activate("run-a"));
        const RunFolder folder = RunFolder::open(m_dir.filePath("runs"), "run-a").value();
        folder.appendEvent({{"event", "node.running"}, {"node", "gen"}});
        folder.appendEvent({{"event", "node.progress"}, {"node", "gen"}, {"value", 0.25}});
        folder.appendEvent({{"event", "node.progress"}, {"node", "gen"}, {"value", 0.5}});
        RunQueue list(paths());
        QCOMPARE(list.entries().first().toMap().value("progress").toDouble(), -1.0); // not read while closed
        list.setWatching(true);
        const QVariantMap row = list.entries().first().toMap();
        QCOMPARE(row.value("node").toString(), QStringLiteral("gen"));
        QCOMPARE(row.value("progress").toDouble(), 0.5);
        folder.appendEvent({{"event", "node.done"}, {"node", "gen"}});
        list.refresh();
        QCOMPARE(list.entries().first().toMap().value("node").toString(), QString());
    }

    void openingARunsFolderHandsItsUrlToTheOpener() {
        queued("run-a", "/w/a.fugue");
        RunQueue list(paths());
        QUrl opened;
        list.setOpenUrl([&](const QUrl &url) { opened = url; });
        list.openFolder("run-a");
        QCOMPARE(opened, QUrl::fromLocalFile(m_dir.filePath("runs/run-a")));
        QCOMPARE(list.workflowOf("run-a"), QStringLiteral("/w/a.fugue"));
    }
};

QTEST_MAIN(TestRunQueue)
#include "test_run_queue.moc"
