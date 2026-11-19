// The run folder of spec 9.7 and run ids.
//
// Seams: newRunId(), and RunFolder (create, open, state, compiled graph, events,
// node folders), read back through the same interface or as the files that
// another process (`fugue runs ...`) would read.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QThread>
#include <QtTest>

#include <atomic>

#include "engine/json.h"
#include "engine/run_folder.h"

using namespace fugue;

class TestRunFolder : public QObject {
    Q_OBJECT

    static int runDones(const RunFolder &folder) {
        int count = 0;
        for (const QJsonObject &event : folder.events())
            count += event.value("event").toString() == QLatin1String("run.done") ? 1 : 0;
        return count;
    }

private slots:
    void runIdsAreOpaqueUniqueAndSortByStartTime() {
        const QRegularExpression form(QStringLiteral("^[a-z0-9-]+$"));
        QStringList ids;
        for (int i = 0; i < 2000; ++i)
            ids << newRunId();
        for (const QString &id : ids)
            QVERIFY2(form.match(id).hasMatch(), qPrintable(id));
        QStringList sorted = ids;
        sorted.sort();
        QCOMPARE(sorted, ids);
        QCOMPARE(QSet<QString>(ids.begin(), ids.end()).size(), ids.size());
    }

    void createMakesTheLayout() {
        QTemporaryDir root;
        const Result<RunFolder> run = RunFolder::create(root.path());
        QVERIFY2(run.ok(), qPrintable(run.error().message));
        QCOMPARE(run->path(), root.filePath(run->id()));
        QVERIFY(QDir(run->path() + "/nodes").exists());
        QCOMPARE(run->runJsonPath(), run->path() + "/run.json");
        QCOMPARE(run->compiledJsonPath(), run->path() + "/compiled.json");
        QCOMPARE(run->eventsPath(), run->path() + "/events.jsonl");
    }

    void aRunIdIsNeverReused() {
        QTemporaryDir root;
        const Result<RunFolder> first = RunFolder::create(root.path(), "20261119-000000-000-aaaa");
        QVERIFY(first.ok());
        const Result<RunFolder> again = RunFolder::create(root.path(), "20261119-000000-000-aaaa");
        QVERIFY(!again.ok());
        QVERIFY(!RunFolder::create(root.path(), "Bad Id").ok());
        QVERIFY(!RunFolder::create(root.path(), "../x").ok());
    }

    void openFindsAnExistingRun() {
        QTemporaryDir root;
        const Result<RunFolder> run = RunFolder::create(root.path());
        QVERIFY(run.ok());
        const Result<RunFolder> opened = RunFolder::open(root.path(), run->id());
        QVERIFY(opened.ok());
        QCOMPARE(opened->path(), run->path());
        const Result<RunFolder> missing = RunFolder::open(root.path(), "nope");
        QVERIFY(!missing.ok());
        QCOMPARE(missing.error().code, QStringLiteral("unknown_run"));
    }

    void stateAndCompiledGraphAreWrittenInTheOnDiskForm() {
        QTemporaryDir root;
        Result<RunFolder> run = RunFolder::create(root.path());
        QVERIFY(run.ok());
        const QJsonObject state{{"status", "running"}, {"pid", 4242}};
        const QJsonObject compiled{{"fugue-compiled", 1}, {"nodes", QJsonObject{}}};
        QVERIFY(run->writeState(state));
        QVERIFY(run->writeCompiled(compiled));

        QFile file(run->runJsonPath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{\n  \"pid\": 4242,\n  \"status\": \"running\"\n}\n"));
        QCOMPARE(run->state(), state);
        QCOMPARE(run->compiled(), compiled);

        QVERIFY(run->writeState(QJsonObject{{"status", "ok"}}));
        QCOMPARE(run->state().value("status").toString(), QStringLiteral("ok"));
    }

    // The workflow a Save Image embeds comes from workflow.json: a write that fails says so,
    // so a run never saves images without their workflow unnoticed.
    void theWorkflowIsReadBackAndAFailedWriteSaysWhy() {
        QTemporaryDir root;
        Result<RunFolder> run = RunFolder::create(root.path());
        QVERIFY(run.ok());
        const QJsonObject document{{"fugue", 1}, {"graph", QJsonObject{{"nodes", QJsonObject{}}}}};
        QVERIFY(run->writeWorkflow(document).isEmpty());
        QCOMPARE(run->workflow(), document);

        QFile::remove(run->path() + "/workflow.json");
        QVERIFY(QFile::setPermissions(run->path(), QFile::ReadOwner | QFile::ExeOwner));
        const Errors failed = run->writeWorkflow(document);
        QVERIFY(QFile::setPermissions(run->path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(hasErrors(failed));
        QCOMPARE(failed.first().code, QStringLiteral("permission_denied"));
        QVERIFY(failed.first().message.contains("workflow.json"));
    }

    void eventsAreOneCompactLineEachWithRunAndTime() {
        QTemporaryDir root;
        Result<RunFolder> run = RunFolder::create(root.path());
        QVERIFY(run.ok());
        QVERIFY(run->appendEvent(QJsonObject{{"event", "run.started"}}));
        QVERIFY(run->appendEvent(QJsonObject{{"event", "node.done"}, {"node", "g7/first"},
                                             {"time", "2026-11-19T10:00:00.000Z"}}));

        QFile file(run->eventsPath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QList<QByteArray> lines = file.readAll().split('\n');
        QCOMPARE(lines.size(), 3); // two lines and the empty rest after the last newline
        QVERIFY(lines.at(2).isEmpty());
        const QJsonObject first = QJsonDocument::fromJson(lines.at(0)).object();
        QCOMPARE(first.value("event").toString(), QStringLiteral("run.started"));
        QCOMPARE(first.value("run").toString(), run->id());
        QVERIFY(QRegularExpression(QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}Z$"))
                    .match(first.value("time").toString())
                    .hasMatch());

        const QList<QJsonObject> events = run->events();
        QCOMPARE(events.size(), 2);
        QCOMPARE(events.at(1).value("node").toString(), QStringLiteral("g7/first"));
        QCOMPARE(events.at(1).value("time").toString(), QStringLiteral("2026-11-19T10:00:00.000Z"));
    }

    void aRunStatusHasANameAndIsReadBack() {
        for (const RunStatus status : {RunStatus::Queued, RunStatus::Running, RunStatus::Ok, RunStatus::Failed,
                                       RunStatus::Cancelled, RunStatus::Unknown})
            QCOMPARE(parseRunStatus(runStatusName(status)), status);
        QCOMPARE(runStatusName(RunStatus::Cancelled), QStringLiteral("cancelled"));
        QCOMPARE(parseRunStatus("finished"), RunStatus::Unknown);
        QVERIFY(isTerminal(RunStatus::Ok) && isTerminal(RunStatus::Failed) && isTerminal(RunStatus::Cancelled));
        QVERIFY(!isTerminal(RunStatus::Queued) && !isTerminal(RunStatus::Running) && !isTerminal(RunStatus::Unknown));
    }

    void aQueuedRunHasItsStatusItsWorkflowAndTheQueuedEvent() {
        QTemporaryDir root;
        const RunFolder folder = RunFolder::create(root.path()).value();
        QCOMPARE(folder.status(), RunStatus::Unknown); // nothing written yet
        QVERIFY(folder.queued("/w/fox.fugue"));
        QCOMPARE(folder.state(), (QJsonObject{{"status", "queued"}, {"workflow", QJsonObject{{"path", "/w/fox.fugue"}}}}));
        QCOMPARE(folder.events().size(), 1);
        QCOMPARE(folder.events().first().value("event").toString(), QStringLiteral("run.queued"));
        QCOMPARE(folder.status(), RunStatus::Queued);
    }

    void finishEndsARunKeepingWhatRunJsonHolds() {
        QTemporaryDir root;
        const RunFolder folder = RunFolder::create(root.path()).value();
        QVERIFY(folder.queued("/w/fox.fugue"));
        const QDateTime at(QDate(2026, 11, 19), QTime(12, 30, 1, 250), QTimeZone::UTC);
        const QJsonObject done = folder.finish(RunStatus::Failed, {makeError("internal", "The run's worker is gone.")}, at);

        const QJsonObject state = folder.state();
        QCOMPARE(state.value("status").toString(), QStringLiteral("failed"));
        QCOMPARE(state.value("ended").toString(), QStringLiteral("2026-11-19T12:30:01.250Z"));
        QCOMPARE(state.value("workflow").toObject().value("path").toString(), QStringLiteral("/w/fox.fugue"));
        const QJsonObject last = folder.events().last();
        QCOMPARE(last.value("event").toString(), QStringLiteral("run.done"));
        QCOMPARE(last.value("status").toString(), QStringLiteral("failed"));
        QCOMPARE(last.value("errors").toArray().at(0).toObject().value("code").toString(), QStringLiteral("internal"));
        QCOMPARE(done.value("status"), last.value("status"));
        QCOMPARE(done.value("errors"), last.value("errors"));
        QCOMPARE(folder.status(), RunStatus::Failed);
    }

    // A run is ended once: a second finish (a follower and a canceller that both found
    // the worker gone) leaves the first end as it is and hands back its run.done.
    void aSecondFinishLeavesTheFirstEndAlone() {
        QTemporaryDir root;
        const RunFolder folder = RunFolder::create(root.path()).value();
        QVERIFY(folder.queued("/w/fox.fugue"));
        const QDateTime first(QDate(2026, 11, 19), QTime(12, 0), QTimeZone::UTC);
        const QJsonObject done = folder.finish(RunStatus::Cancelled, {}, first);
        const QJsonObject again =
            RunFolder::open(root.path(), folder.id())
                ->finish(RunStatus::Failed, {makeError("internal", "The run's worker is gone.")}, first.addSecs(5));

        QCOMPARE(runDones(folder), 1);
        QCOMPARE(folder.state().value("status").toString(), QStringLiteral("cancelled"));
        QCOMPARE(folder.state().value("ended").toString(), QStringLiteral("2026-11-19T12:00:00.000Z"));
        QCOMPARE(again.value("status").toString(), QStringLiteral("cancelled"));
        QCOMPARE(again.value("errors"), done.value("errors"));
        QCOMPARE(folder.status(), RunStatus::Cancelled);
    }

    // Finishers that race: each RunFolder opens its own files, as another process would.
    // Whatever the interleaving, every run has one run.done and run.json agrees with it.
    void finishersThatRaceEndARunOnce() {
        QTemporaryDir root;
        constexpr int runs = 40;
        constexpr int finishers = 4;
        QStringList ids;
        for (int i = 0; i < runs; ++i) {
            const RunFolder folder = RunFolder::create(root.path()).value();
            QVERIFY(folder.queued());
            ids << folder.id();
        }
        std::atomic<int> ready{0};
        QList<QThread *> threads;
        for (int f = 0; f < finishers; ++f) {
            const RunStatus status = f % 2 ? RunStatus::Failed : RunStatus::Cancelled;
            threads << QThread::create([&, status] {
                ++ready;
                while (ready.load() < finishers) {
                }
                for (const QString &id : std::as_const(ids))
                    RunFolder::open(root.path(), id)->finish(status, {}, QDateTime::currentDateTimeUtc());
            });
        }
        for (QThread *thread : std::as_const(threads))
            thread->start();
        for (QThread *thread : std::as_const(threads)) {
            QVERIFY(thread->wait());
            delete thread;
        }
        for (const QString &id : std::as_const(ids)) {
            const RunFolder folder = RunFolder::open(root.path(), id).value();
            QCOMPARE(runDones(folder), 1);
            QCOMPARE(folder.state().value("status").toString(), runStatusName(folder.status()));
        }
    }

    void theStatusIsRunDonesOnceTheRunHasEnded() {
        QTemporaryDir root;
        const RunFolder folder = RunFolder::create(root.path()).value();
        folder.appendEvent(QJsonObject{{"event", "run.queued"}});
        QCOMPARE(folder.status(), RunStatus::Queued); // from the events, before any run.json
        folder.appendEvent(QJsonObject{{"event", "run.started"}});
        QCOMPARE(folder.status(), RunStatus::Running);
        folder.writeState(QJsonObject{{"status", "running"}});
        folder.appendEvent(QJsonObject{{"event", "run.done"}, {"status", "cancelled"}});
        // A run.json written late does not bring an ended run back.
        QCOMPARE(folder.status(), RunStatus::Cancelled);
    }

    void aTornLastEventIsSkipped() {
        QTemporaryDir root;
        Result<RunFolder> run = RunFolder::create(root.path());
        QVERIFY(run.ok());
        QVERIFY(run->appendEvent(QJsonObject{{"event", "run.started"}}));
        QFile file(run->eventsPath());
        QVERIFY(file.open(QIODevice::Append));
        file.write("{\"event\": \"node.do");
        file.close();
        QCOMPARE(run->events().size(), 1);
    }

    void nodeFoldersFollowThePathId() {
        QTemporaryDir root;
        Result<RunFolder> run = RunFolder::create(root.path());
        QVERIFY(run.ok());
        const Result<QString> top = run->nodeDir("gen");
        QVERIFY(top.ok());
        QCOMPARE(*top, run->path() + "/nodes/gen");
        QVERIFY(QDir(*top).exists());
        const Result<QString> inner = run->nodeDir("g7/inner3/s2");
        QVERIFY(inner.ok());
        QCOMPARE(*inner, run->path() + "/nodes/g7/inner3/s2");
        QVERIFY(QDir(*inner).exists());

        for (const char *bad : {"", "..", "../x", "a/../b", "/abs", "a//b", "a/", "Gen", "a.b", "9x"}) {
            const Result<QString> dir = run->nodeDir(QString::fromLatin1(bad));
            QVERIFY2(!dir.ok(), bad);
        }
    }

    void defaultRootIsUnderTheCacheFolder() {
        QVERIFY(RunFolder::defaultRoot().endsWith(QStringLiteral("/fugue/runs")));
    }
};

QTEST_GUILESS_MAIN(TestRunFolder)
#include "test_run_folder.moc"
