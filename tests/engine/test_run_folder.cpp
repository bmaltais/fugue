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
#include <QtTest>

#include "engine/json.h"
#include "engine/run_folder.h"

using namespace fugue;

class TestRunFolder : public QObject {
    Q_OBJECT

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
