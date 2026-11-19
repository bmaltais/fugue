// The run queue folder and the engine lock (spec 9.8).
//
// Seam: Queue and Lock on a temporary folder. Two Queue objects on the same folder stand
// for two processes: the folder is all they share. Two Lock objects on the same file
// exclude each other the way two processes do, because each has its own open file.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <csignal>

#include "engine/json.h"
#include "engine/queue.h"

using namespace fugue;

class TestQueue : public QObject {
    Q_OBJECT

    static QStringList runs(const Queue &queue) {
        QStringList list;
        for (const QueueEntry &entry : queue.entries())
            list << entry.run;
        return list;
    }

private slots:
    void entriesComeInArrivalOrderAndFirstGoesAheadOfTheWaitingOnes() {
        QTemporaryDir dir;
        const Queue queue(dir.filePath("queue"));
        QVERIFY(queue.enqueue("run-a", "/w/a.fugue", false));
        QVERIFY(queue.activate("run-a"));
        QVERIFY(queue.enqueue("run-b", "/w/b.fugue", false));
        QVERIFY(queue.enqueue("run-c", "/w/c.fugue", true));
        // The active run stays first; "first" only passes the waiting ones.
        QCOMPARE(runs(queue), (QStringList{"run-a", "run-c", "run-b"}));
        const QList<QueueEntry> entries = queue.entries();
        QVERIFY(entries.at(0).active);
        QVERIFY(!entries.at(1).active);
        QCOMPARE(entries.at(2).workflow, QString("/w/b.fugue"));
    }

    void anEntryIsWrittenInTheOnDiskJsonForm() {
        QTemporaryDir dir;
        const Queue queue(dir.filePath("queue"));
        QVERIFY(queue.enqueue("run-a", "/w/a.fugue", false));
        QVERIFY(queue.setPid("run-a", 4242));
        QueueEntry entry;
        QVERIFY(queue.find("run-a", &entry));
        QCOMPARE(entry.pid, qint64(4242));
        QFile file(dir.filePath("queue/" + entry.file));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), writeJson(QJsonObject{{"pid", 4242}, {"run", "run-a"}, {"workflow", "/w/a.fugue"}}));
    }

    void aPidIsNotRecordedForAnEntryThatWasRemoved() {
        QTemporaryDir dir;
        const Queue queue(dir.filePath("queue"));
        QVERIFY(queue.enqueue("run-a", "/w/a.fugue", false));
        QVERIFY(queue.remove("run-a"));
        QVERIFY(!queue.setPid("run-a", 4242)); // a cancelled entry stays gone
        QVERIFY(queue.entries().isEmpty());
    }

    // The race of a cancel against the worker's activation: the cancel read the entry
    // while it waited; the worker then renamed it to the active class.
    void removingTheEntryThatWasSeenFailsOnceTheRunWasActivated() {
        QTemporaryDir dir;
        const Queue canceller(dir.filePath("queue"));
        const Queue worker(dir.filePath("queue"));
        QVERIFY(canceller.enqueue("run-a", "/w/a.fugue", false));
        QueueEntry seen;
        QVERIFY(canceller.find("run-a", &seen));
        QVERIFY(worker.activate("run-a"));

        QVERIFY(!canceller.remove(seen));
        QueueEntry now;
        QVERIFY(canceller.find("run-a", &now)); // the active entry is untouched
        QVERIFY(now.active);
    }

    void removingTheEntryThatWasSeenWinsOverALaterActivation() {
        QTemporaryDir dir;
        const Queue canceller(dir.filePath("queue"));
        const Queue worker(dir.filePath("queue"));
        QVERIFY(canceller.enqueue("run-a", "/w/a.fugue", false));
        QueueEntry seen;
        QVERIFY(canceller.find("run-a", &seen));
        QVERIFY(canceller.remove(seen));
        QVERIFY(!worker.activate("run-a"));
        QVERIFY(!canceller.remove(seen)); // only one process gets it
    }

    void onlyWholeEntriesAreListed() {
        QTemporaryDir dir;
        const Queue queue(dir.filePath("queue"));
        QVERIFY(queue.enqueue("run-a", "/w/a.fugue", false));
        // What an atomic write leaves for a moment, and a stray file.
        for (const char *name : {"5-00000000000001-run-b.AbCdEf", "notes.txt"}) {
            QFile file(dir.filePath(QStringLiteral("queue/") + name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QCOMPARE(runs(queue), (QStringList{"run-a"}));
    }

    void theLockIsHeldByOneHolderAtATime() {
        QTemporaryDir dir;
        Lock one(dir.filePath("engine.lock"));
        Lock other(dir.filePath("engine.lock"));
        QVERIFY(one.tryAcquire());
        QVERIFY(one.tryAcquire()); // already held
        QVERIFY(!other.tryAcquire());
        one.release();
        QVERIFY(!one.held());
        QVERIFY(other.tryAcquire());
    }

    // 9.8: "An entry whose process is gone is discarded."
    void anActiveEntryIsDeadExactlyWhenTheEngineLockIsFree() {
        QTemporaryDir dir;
        const Queue queue(dir.filePath("queue"));
        QVERIFY(queue.enqueue("run-a", "/w/a.fugue", false));
        QVERIFY(queue.activate("run-a"));

        Lock owner(dir.filePath("engine.lock")); // the worker of run-a, alive
        QVERIFY(owner.tryAcquire());
        Lock waiter(dir.filePath("engine.lock"));
        QVERIFY(queue.discardDead(waiter).isEmpty());
        QCOMPARE(runs(queue), (QStringList{"run-a"}));

        owner.release(); // what the system does when the worker dies
        QCOMPARE(queue.discardDead(waiter), (QStringList{"run-a"}));
        QVERIFY(queue.entries().isEmpty());
        QVERIFY(!waiter.held()); // left as it was found
    }

    void aWaitingEntryIsDeadWhenItsPidIsNotThatRunsWorker() {
        QTemporaryDir dir;
        const Queue queue(dir.filePath("queue"));
        QVERIFY(queue.enqueue("run-a", "/w/a.fugue", false));
        QVERIFY(queue.enqueue("run-b", "/w/b.fugue", false));
        // This test process is alive, but it is not `fugue __worker run-a`: a reused pid.
        QVERIFY(queue.setPid("run-a", QCoreApplication::applicationPid()));
        Lock lock(dir.filePath("engine.lock"));
        QCOMPARE(queue.discardDead(lock), (QStringList{"run-a"}));
        // run-b has no pid yet and is new: its worker is still starting.
        QCOMPARE(runs(queue), (QStringList{"run-b"}));
    }

    void aProcessIsARunsWorkerOnlyByItsCommandLine() {
        QVERIFY(!isRunWorker(QCoreApplication::applicationPid(), "run-a"));
        QVERIFY(!isRunWorker(0, "run-a"));
        QProcess worker; // any program whose arguments are `__worker run-a`
        worker.start("sh", {"-c", "read line", "__worker", "run-a"});
        QVERIFY(worker.waitForStarted());
        // Just after it starts, a process has no command line for a moment (the kernel
        // sets it late in exec), which is why only a worker records its own pid.
        QTRY_VERIFY(isRunWorker(worker.processId(), "run-a"));
        QVERIFY(!isRunWorker(worker.processId(), "run-b"));
        worker.closeWriteChannel();
        QVERIFY(worker.waitForFinished());
        QVERIFY(!isRunWorker(worker.processId(), "run-a"));
    }

    // A cancel signals the worker it checked, never a process that got its id since:
    // the signal goes through a pidfd opened before the check. (The reuse window itself
    // cannot be produced in a test; this pins down what the call promises.)
    void onlyTheRunsWorkerIsSignalled() {
        QVERIFY(!signalRunWorker(0, "run-a", SIGTERM));
        QProcess other; // alive, but the worker of another run
        other.start("sh", {"-c", "read line", "__worker", "run-b"});
        QVERIFY(other.waitForStarted());
        QTRY_VERIFY(isRunWorker(other.processId(), "run-b"));
        QVERIFY(!signalRunWorker(other.processId(), "run-a", SIGTERM));
        other.closeWriteChannel();
        QVERIFY(other.waitForFinished());
        QCOMPARE(other.exitStatus(), QProcess::NormalExit); // it was not signalled

        QProcess worker;
        worker.start("sh", {"-c", "read line", "__worker", "run-a"});
        QVERIFY(worker.waitForStarted());
        QTRY_VERIFY(isRunWorker(worker.processId(), "run-a"));
        QVERIFY(signalRunWorker(worker.processId(), "run-a", SIGTERM));
        QVERIFY(worker.waitForFinished());
        QCOMPARE(worker.exitStatus(), QProcess::CrashExit); // ended by the signal
        QVERIFY(!signalRunWorker(worker.processId(), "run-a", SIGTERM)); // gone
    }
};

QTEST_GUILESS_MAIN(TestQueue)
#include "test_queue.moc"
