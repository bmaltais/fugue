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
#include <ctime>

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

    // CLOCK_BOOTTIME in milliseconds: the same clock in every process, unmoved by changes
    // to the wall clock, and counting suspend.
    static qint64 bootMs() {
        timespec now{};
        clock_gettime(CLOCK_BOOTTIME, &now);
        return qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    }

    // An entry as some process wrote it, in the on-disk form.
    static void writeEntry(const QTemporaryDir &dir, const QString &name, const QJsonObject &content) {
        QDir().mkpath(dir.filePath("queue"));
        QVERIFY(writeJsonFile(dir.filePath("queue/" + name), content).isEmpty());
    }

    static void age(const QTemporaryDir &dir, const QString &name, int seconds) {
        QFile file(dir.filePath("queue/" + name));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-seconds), QFileDevice::FileModificationTime));
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
        const qint64 before = bootMs();
        QVERIFY(queue.enqueue("run-a", "/w/a.fugue", false));
        const qint64 after = bootMs();
        QVERIFY(queue.setPid("run-a", 4242));
        QueueEntry entry;
        QVERIFY(queue.find("run-a", &entry));
        QCOMPARE(entry.pid, qint64(4242));
        QFile file(dir.filePath("queue/" + entry.file));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        // When it was queued, on the boot clock; setPid keeps it.
        const qint64 queued = qint64(QJsonDocument::fromJson(bytes).object().value("queued_boottime_ms").toDouble());
        QVERIFY(before <= queued && queued <= after);
        QCOMPARE(bytes, writeJson(QJsonObject{{"pid", 4242}, {"queued_boottime_ms", double(queued)}, {"run", "run-a"},
                                              {"workflow", "/w/a.fugue"}}));
    }

    // The 30 s allowed for a new worker to record its pid are measured on the boot clock,
    // so a jump of the wall clock does not discard a worker that is still starting.
    void aNewEntryWithoutAPidIsKeptWhenTheWallClockJumps() {
        QTemporaryDir dir;
        const Queue queue(dir.filePath("queue"));
        QVERIFY(queue.enqueue("run-a", "/w/a.fugue", false));
        age(dir, queue.entries().first().file, 3600); // the file looks an hour old
        Lock lock(dir.filePath("engine.lock"));
        QVERIFY(queue.discardDead(lock).isEmpty());
        QCOMPARE(runs(queue), (QStringList{"run-a"}));
    }

    void anEntryThatGotNoPidIn30SecondsIsDead() {
        QTemporaryDir dir;
        const qint64 now = bootMs();
        writeEntry(dir, "5-00000000000001-run-old", {{"run", "run-old"}, {"pid", 0}, {"queued_boottime_ms", double(now - 31000)}});
        writeEntry(dir, "5-00000000000002-run-new", {{"run", "run-new"}, {"pid", 0}, {"queued_boottime_ms", double(now - 1000)}});
        // Later than now on the boot clock: queued before the machine restarted.
        writeEntry(dir, "5-00000000000003-run-before-boot",
                   {{"run", "run-before-boot"}, {"pid", 0}, {"queued_boottime_ms", double(now + 3600000)}});
        const Queue queue(dir.filePath("queue"));
        Lock lock(dir.filePath("engine.lock"));
        QCOMPARE(queue.discardDead(lock), (QStringList{"run-old", "run-before-boot"}));
        QCOMPARE(runs(queue), (QStringList{"run-new"}));
    }

    // Entries written before the boot time was recorded fall back to the file's time.
    void anEntryWithoutTheBootTimeIsAgedByItsFile() {
        QTemporaryDir dir;
        writeEntry(dir, "5-00000000000001-run-old", {{"run", "run-old"}, {"pid", 0}});
        writeEntry(dir, "5-00000000000002-run-new", {{"run", "run-new"}, {"pid", 0}});
        age(dir, "5-00000000000001-run-old", 31);
        const Queue queue(dir.filePath("queue"));
        Lock lock(dir.filePath("engine.lock"));
        QCOMPARE(queue.discardDead(lock), (QStringList{"run-old"}));
        QCOMPARE(runs(queue), (QStringList{"run-new"}));
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

    // The window runs its own runs (9.8): its entries are not a `__worker`'s, so they are
    // live while the window's process lives, known by its pid and that process's start time.
    void aWindowsWaitingEntryIsLiveWhileItsProcessLives() {
        QTemporaryDir dir;
        const Queue queue(dir.filePath("queue"));
        QVERIFY(queue.enqueueForThisProcess("run-a", "/w/a.fugue", false));
        QueueEntry entry;
        QVERIFY(queue.find("run-a", &entry));
        QCOMPARE(entry.pid, QCoreApplication::applicationPid());
        QVERIFY(entry.inProcess);
        Lock lock(dir.filePath("engine.lock"));
        QVERIFY(queue.discardDead(lock).isEmpty());
        QCOMPARE(runs(queue), (QStringList{"run-a"}));
    }

    void aWindowsWaitingEntryIsDeadOnceItsPidBelongsToAnotherProcess() {
        QTemporaryDir dir;
        // This process's pid, but a start time that is not this process's: the window died
        // and the id went to someone else.
        writeEntry(dir, "5-00000000000001-run-a",
                   {{"run", "run-a"}, {"pid", double(QCoreApplication::applicationPid())}, {"owner", "window"},
                    {"process_start", 1}, {"queued_boottime_ms", double(bootMs())}});
        const Queue queue(dir.filePath("queue"));
        Lock lock(dir.filePath("engine.lock"));
        QCOMPARE(queue.discardDead(lock), (QStringList{"run-a"}));
    }

    // The window holds the engine lock while its run is active; looking for dead entries
    // then must not take its own active entry for one.
    void aProcessHoldingTheLockKeepsItsOwnActiveEntry() {
        QTemporaryDir dir;
        const Queue queue(dir.filePath("queue"));
        QVERIFY(queue.enqueueForThisProcess("run-a", "/w/a.fugue", false));
        QVERIFY(queue.activate("run-a"));
        Lock lock(dir.filePath("engine.lock"));
        QVERIFY(lock.tryAcquire());
        QVERIFY(queue.discardDead(lock).isEmpty());
        QCOMPARE(runs(queue), (QStringList{"run-a"}));
        QVERIFY(lock.held());
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
        const qint64 pid = worker.processId(); // QProcess forgets it once the process ends
        worker.closeWriteChannel();
        QVERIFY(worker.waitForFinished());
        QVERIFY(!isRunWorker(pid, "run-a"));
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
        const qint64 pid = worker.processId(); // QProcess forgets it once the process ends
        QVERIFY(signalRunWorker(pid, "run-a", SIGTERM));
        QVERIFY(worker.waitForFinished());
        QCOMPARE(worker.exitStatus(), QProcess::CrashExit); // ended by the signal
        QVERIFY(!signalRunWorker(pid, "run-a", SIGTERM)); // gone
    }
};

QTEST_GUILESS_MAIN(TestQueue)
#include "test_queue.moc"
