// The run queue inside a process (spec 9.2): one run at a time, first in
// first out, "run first", and remove or cancel by id.
//
// Seam: RunQueue's public interface, observed through its signals and the
// run folders. Runs are graphs of fake nodes.

#include <QSemaphore>
#include <QtTest>

#include "engine/run_queue.h"
#include "scheduler_support.h"

using namespace fugue;
using namespace fugue::test;

class TestRunQueue : public QObject {
    Q_OBJECT

    static CompiledGraph graphOf(World &world, const QString &text) {
        return world.graph(QJsonObject{
            {"a", QJsonObject{{"type", "fugue/text"}, {"inputs", QJsonObject{{"text", text}}}}},
            {"s", QJsonObject{{"type", "fugue/save"}, {"inputs", QJsonObject{{"value", QJsonObject{{"from", "a.text"}}}}}}}});
    }

    // The run.* events of every run, as "<name> <event>" in the order they came.
    static void watch(RunQueue &queue, QMap<QString, QString> &names, QStringList &seen) {
        connect(&queue, &RunQueue::runEvent, &queue, [&](const QString &id, const QJsonObject &event) {
            const QString name = event.value("event").toString();
            if (name.startsWith("run."))
                seen.append(names.value(id, id) + " " + name);
        });
    }

private slots:
    void runsGoOneAtATimeInTheOrderTheyWereQueued() {
        World world;
        RunQueue queue(world.environment());
        QMap<QString, QString> names;
        QStringList seen;
        watch(queue, names, seen);
        QStringList finished;
        connect(&queue, &RunQueue::runFinished, this, [&](const QString &id, const RunOutcome &outcome) {
            finished.append(names.value(id) + " " + outcome.status);
        });

        const RunFolder one = world.folder();
        const RunFolder two = world.folder();
        const RunFolder three = world.folder();
        names = {{one.id(), "one"}, {two.id(), "two"}, {three.id(), "three"}};
        QCOMPARE(queue.enqueue(graphOf(world, "1"), one), one.id());
        queue.enqueue(graphOf(world, "2"), two);
        queue.enqueue(graphOf(world, "3"), three);
        QCOMPARE(queue.active(), one.id());
        QCOMPARE(queue.waiting(), (QStringList{two.id(), three.id()}));
        QCOMPARE(two.state().value("status").toString(), QStringLiteral("queued"));

        QTRY_COMPARE(finished.size(), 3);
        QCOMPARE(finished, (QStringList{"one ok", "two ok", "three ok"}));
        QCOMPARE(seen, (QStringList{"one run.queued", "one run.started", "two run.queued", "three run.queued",
                                    "one run.done", "two run.started", "two run.done", "three run.started",
                                    "three run.done"}));
        QVERIFY(queue.active().isEmpty());
        QVERIFY(queue.waiting().isEmpty());
        QCOMPARE(trace(three.events()).first(), QStringLiteral("run.queued"));
        QCOMPARE(three.state().value("status").toString(), QStringLiteral("ok"));
    }

    void aRunCanGoFirstAndAWaitingRunCanBeRemoved() {
        World world;
        RunQueue queue(world.environment());
        QStringList started;
        connect(&queue, &RunQueue::runStarted, this, [&](const QString &id) { started.append(id); });
        QStringList finished;
        connect(&queue, &RunQueue::runFinished, this, [&](const QString &id, const RunOutcome &outcome) {
            finished.append(id + " " + outcome.status);
        });

        const RunFolder one = world.folder();
        const RunFolder two = world.folder();
        const RunFolder three = world.folder();
        const RunFolder four = world.folder();
        const RunFolder five = world.folder();
        queue.enqueue(graphOf(world, "1"), one);
        queue.enqueue(graphOf(world, "2"), two);
        queue.enqueue(graphOf(world, "3"), three);
        queue.enqueue(graphOf(world, "4"), four, {}, true); // queued to run first
        queue.enqueue(graphOf(world, "5"), five);
        QVERIFY(queue.moveToFront(five.id()));
        QVERIFY(!queue.moveToFront(one.id())); // already running
        QCOMPARE(queue.waiting(), (QStringList{five.id(), four.id(), two.id(), three.id()}));

        // Removing a waiting run ends it as cancelled without running it.
        QVERIFY(queue.cancel(two.id()));
        QVERIFY(!queue.cancel("no-such-run"));
        QCOMPARE(finished, QStringList{two.id() + " cancelled"});
        QCOMPARE(two.state().value("status").toString(), QStringLiteral("cancelled"));
        QCOMPARE(trace(two.events()), (QStringList{"run.queued", "run.done cancelled"}));

        QTRY_COMPARE(finished.size(), 5);
        QCOMPARE(started, (QStringList{one.id(), five.id(), four.id(), three.id()}));
        QCOMPARE(world.calls.count("a"), 4);
    }

    void theActiveRunCanBeCancelledAndTheNextStillRuns() {
        World world;
        QSemaphore entered;
        QSemaphore released;
        world.kernel("fugue/text", [&](const NodeContext &context) -> Result<Values> {
            entered.release();
            released.acquire();
            return Values{{"text", context.input("text")}};
        });
        RunQueue queue(world.environment());
        QStringList finished;
        connect(&queue, &RunQueue::runFinished, this, [&](const QString &id, const RunOutcome &outcome) {
            finished.append(id + " " + outcome.status);
        });
        const RunFolder one = world.folder();
        const RunFolder two = world.folder();
        queue.enqueue(graphOf(world, "1"), one);
        queue.enqueue(graphOf(world, "2"), two);
        QTRY_VERIFY(entered.available() > 0);
        QVERIFY(queue.cancel(one.id()));
        released.release(2);
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(finished, (QStringList{one.id() + " cancelled", two.id() + " ok"}));
    }
};

QTEST_GUILESS_MAIN(TestRunQueue)
#include "test_run_queue.moc"
