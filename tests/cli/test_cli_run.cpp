// `fugue run` and what surrounds it (spec 9.8, 13.1): the queue folder and engine lock,
// the detached worker, --wait, cancel, --set and the garbage collection a worker does.
//
// Seam: the `fugue` command line as a subprocess, with no display. The binary is
// fugue_with_fakes: the same CLI, queue, lock and worker code as `fugue`, with fake node
// kernels registered through cli::Hooks. That is the more honest choice here. The
// production binary has no kernels until M1, so through it a run can only fail with "no
// kernel"; a fake kernel table behind an env var would ship test code in the app, and
// testing only the no-kernel failure would leave the worker, queue order and cancel
// untested. One test still drives the real `fugue` to pin the no-kernel failure.
// Nothing sleeps for a fixed time: the `hold` kernel waits on a gate file, and tests wait
// for observable states in the run folder.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <csignal>

#include "engine/run_folder.h"

using namespace fugue;

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QJsonObject data() const { return json().value("data").toObject(); }
    QString firstCode() const { return json().value("errors").toArray().at(0).toObject().value("code").toString(); }
    QList<QJsonObject> lines() const {
        QList<QJsonObject> list;
        for (const QByteArray &line : out.trimmed().split('\n'))
            list.append(QJsonDocument::fromJson(line).object());
        return list;
    }
};

QJsonObject manifest(const QString &id, bool sink, bool textInput) {
    return QJsonObject{
        {"manifest", 1},
        {"id", id},
        {"version", "1.0.0"},
        {"name", id},
        {"runtime", QJsonObject{{"kind", "builtin"}}},
        {"inputs", textInput ? QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}}} : QJsonArray()},
        {"outputs", QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}}}},
        {"sink", sink},
    };
}

QByteArray flow(const QByteArray &nodes) {
    return "{\"fugue\": 1, \"graph\": {\"nodes\": {" + nodes + "}}}";
}

const QByteArray node(const char *id, const char *type, const char *inputs = "{}") {
    return QByteArray("\"") + id + "\": {\"type\": \"" + type + "\", \"version\": \"1.0.0\", \"inputs\": " + inputs + "}";
}

} // namespace

class TestCliRun : public QObject {
    Q_OBJECT

    QTemporaryDir *m_dir = nullptr;

    QString path(const QString &name) const { return m_dir->filePath(name); }
    QString runsRoot() const { return path("cache/fugue/runs"); }
    QString queueDir() const { return path("state/fugue/queue"); }
    QString gate() const { return path("gate"); }

    void write(const QString &file, const QByteArray &bytes) const {
        QDir().mkpath(QFileInfo(file).path());
        QFile handle(file);
        QVERIFY2(handle.open(QIODevice::WriteOnly), qPrintable(file));
        handle.write(bytes);
    }

    Outcome fugue(const QStringList &args, const QString &binary = FUGUE_FAKE_BINARY) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.remove("DISPLAY");
        env.remove("WAYLAND_DISPLAY");
        env.insert("XDG_CACHE_HOME", path("cache"));
        env.insert("XDG_STATE_HOME", path("state"));
        env.insert("XDG_CONFIG_HOME", path("config"));
        env.insert("FUGUE_NODE_MANIFESTS", path("manifests"));
        env.insert("FUGUE_TEST_GATE", gate());
        QProcess process;
        process.setProcessEnvironment(env);
        process.start(binary, args);
        Outcome outcome;
        if (!process.waitForFinished(30000)) {
            process.kill();
            process.waitForFinished();
            qWarning("fugue timed out: %s", qPrintable(args.join(' ')));
            return outcome;
        }
        outcome.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    QString start(const QString &file, const QStringList &more = {}) const {
        const Outcome o = fugue(QStringList{"run", path(file), "--json"} + more);
        if (o.exitCode != 0)
            qWarning("run failed: %s", o.out.constData());
        return o.data().value("run").toString();
    }

    QString statusOf(const QString &id) const {
        const auto folder = RunFolder::open(runsRoot(), id);
        return folder.ok() ? folder->state().value("status").toString() : QString();
    }

    QStringList queued() const { return QDir(queueDir()).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name); }

private slots:
    void init() {
        delete m_dir;
        m_dir = new QTemporaryDir;
        QVERIFY(m_dir->isValid());
        write(path("manifests/note/node.json"), QJsonDocument(manifest("fugue/note", false, false)).toJson());
        write(path("manifests/echo/node.json"), QJsonDocument(manifest("fugue/echo", true, true)).toJson());
        write(path("manifests/hold/node.json"), QJsonDocument(manifest("fugue/hold", true, false)).toJson());
        write(path("manifests/boom/node.json"), QJsonDocument(manifest("fugue/boom", true, false)).toJson());
        write(path("manifests/gen/node.json"), QJsonDocument(manifest("fugue/gen", true, false)).toJson());
        write(path("flow.fugue"), flow(node("a", "fugue/note") + "," + node("b", "fugue/echo", "{\"text\": {\"from\": \"a.text\"}}")));
        write(path("lit.fugue"), flow(node("b", "fugue/echo", "{\"text\": \"original\"}")));
        write(path("hold.fugue"), flow(node("h", "fugue/hold")));
        write(path("boom.fugue"), flow(node("x", "fugue/boom")));
        write(path("gen.fugue"), flow(node("g", "fugue/gen")));
    }

    void cleanup() {
        // Let any worker finish before its folders go away.
        QVERIFY2(QFile(gate()).open(QIODevice::WriteOnly), "gate");
        QTest::qWait(300);
        delete m_dir;
        m_dir = nullptr;
    }

    void runPrintsARunIdAndReturnsWhileAWorkerFinishesTheRun() {
        const Outcome o = fugue({"run", path("flow.fugue"), "--json"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(o.json().value("ok").toBool(), true);
        QCOMPARE(o.data().value("status").toString(), QString("queued"));
        const QString id = o.data().value("run").toString();
        QVERIFY(!id.isEmpty());

        const Outcome events = fugue({"runs", "events", id, "--follow"});
        QCOMPARE(events.exitCode, 0);
        QCOMPARE(events.lines().last().value("event").toString(), QString("run.done"));
        QCOMPARE(events.lines().first().value("event").toString(), QString("run.queued"));

        const Outcome result = fugue({"runs", "result", id, "--json"});
        QCOMPARE(result.data().value("status").toString(), QString("ok"));
        QCOMPARE(result.data().value("outputs").toObject().value("b").toObject().value("text").toString(),
                 QString("hello"));
        QTRY_VERIFY_WITH_TIMEOUT(queued().isEmpty(), 10000); // the worker removed its entry
    }

    void runWaitStreamsEventsAndExitsZero() {
        const Outcome o = fugue({"run", path("flow.fugue"), "--wait", "--json"});
        QCOMPARE(o.exitCode, 0);
        const QList<QJsonObject> lines = o.lines();
        QCOMPARE(lines.first().value("event").toString(), QString("run.queued"));
        QCOMPARE(lines.last().value("event").toString(), QString("run.done"));
        QCOMPARE(lines.last().value("status").toString(), QString("ok"));
    }

    void runWaitExitsOneWhenARunFails() {
        const Outcome o = fugue({"run", path("boom.fugue"), "--wait"});
        QCOMPARE(o.exitCode, 1);
        QCOMPARE(o.lines().last().value("status").toString(), QString("failed"));
    }

    void runWithAnyCompileErrorStartsNothing() {
        write(path("bad.fugue"), flow(node("b", "fugue/echo")));
        const Outcome o = fugue({"run", path("bad.fugue"), "--json"});
        QCOMPARE(o.exitCode, 1);
        QCOMPARE(o.firstCode(), QString("missing_input"));
        QVERIFY(!QDir(runsRoot()).exists() || QDir(runsRoot()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(queued().isEmpty());
    }

    void setOverridesALiteralBeforeCompiling() {
        const Outcome o = fugue({"run", path("lit.fugue"), "--wait", "--set", "b.text=changed"});
        QCOMPARE(o.exitCode, 0);
        QJsonObject done;
        for (const QJsonObject &event : o.lines())
            if (event.value("event").toString() == "node.done")
                done = event;
        QCOMPARE(done.value("outputs").toObject().value("text").toString(), QString("changed"));
        // The overridden value is data, so a number stays a number for compile to check.
        const Outcome bad = fugue({"run", path("lit.fugue"), "--set", "nope.text=1", "--json"});
        QCOMPARE(bad.firstCode(), QString("usage"));
        const Outcome wrong = fugue({"run", path("lit.fugue"), "--set", "b.text=3", "--json"});
        QCOMPARE(wrong.exitCode, 1); // 3 is a number, not text
        QCOMPARE(wrong.firstCode(), QString("type_mismatch"));
    }

    void queueRunsOneAtATimeAndFirstGoesAheadOfWaitingRuns() {
        const QString a = start("hold.fugue");
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(a), QString("running"), 15000);
        const QString b = start("hold.fugue");
        const QString c = start("hold.fugue", {"--first"});
        QVERIFY(!b.isEmpty() && !c.isEmpty());

        // The queue folder holds one entry per run, in the order they will go.
        const QStringList names = queued();
        QCOMPARE(names.size(), 3);
        QVERIFY(names.at(0).endsWith(a) && names.at(1).endsWith(c) && names.at(2).endsWith(b));
        QCOMPARE(statusOf(b), QString("queued"));

        const Outcome list = fugue({"runs", "list", "--json"});
        QMap<QString, int> positions;
        QMap<QString, QString> statuses;
        for (const QJsonValue &run : list.data().value("runs").toArray()) {
            positions.insert(run.toObject().value("id").toString(), run.toObject().value("position").toInt(-1));
            statuses.insert(run.toObject().value("id").toString(), run.toObject().value("status").toString());
        }
        QCOMPARE(positions.value(a), 0);
        QCOMPARE(positions.value(c), 1);
        QCOMPARE(positions.value(b), 2);
        QCOMPARE(statuses.value(b), QString("queued"));

        QVERIFY(QFile(gate()).open(QIODevice::WriteOnly));
        for (const QString &id : {a, b, c})
            QTRY_COMPARE_WITH_TIMEOUT(statusOf(id), QString("ok"), 20000);
        const auto started = [&](const QString &id) {
            return RunFolder::open(runsRoot(), id)->state().value("started").toString();
        };
        QVERIFY(started(a) < started(c));
        QVERIFY(started(c) < started(b));
        QTRY_VERIFY_WITH_TIMEOUT(queued().isEmpty(), 10000);
    }

    void cancelStopsARunningRun() {
        const QString id = start("hold.fugue");
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(id), QString("running"), 15000);
        const Outcome o = fugue({"runs", "cancel", id, "--json"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(o.data().value("status").toString(), QString("cancelling"));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(id), QString("cancelled"), 15000);
        QCOMPARE(fugue({"runs", "events", id, "--follow"}).exitCode, 1);
        QTRY_VERIFY_WITH_TIMEOUT(queued().isEmpty(), 10000);
        // Cancelling a run that has ended changes nothing.
        QCOMPARE(fugue({"runs", "cancel", id, "--json"}).data().value("cancelled").toBool(true), false);
    }

    void cancelRemovesAQueuedRunsEntry() {
        const QString a = start("hold.fugue");
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(a), QString("running"), 15000);
        const QString b = start("hold.fugue");
        QCOMPARE(queued().size(), 2);
        const Outcome o = fugue({"runs", "cancel", b, "--json"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(statusOf(b), QString("cancelled"));
        QCOMPARE(queued().size(), 1);
        QVERIFY(queued().first().endsWith(a));
        QVERIFY(QFile(gate()).open(QIODevice::WriteOnly));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(a), QString("ok"), 20000);
        QCOMPARE(statusOf(b), QString("cancelled")); // its worker did not run it
    }

    void anEntryWhoseProcessIsGoneIsDiscarded() {
        const qint64 dead = 4194303;
        if (::kill(pid_t(dead), 0) == 0)
            QSKIP("pid 4194303 exists on this machine");
        // A crashed worker left itself first in the queue, and its run unfinished.
        auto folder = RunFolder::create(runsRoot(), "20260101-000000-000-dead");
        QVERIFY(folder.ok());
        folder->writeState(QJsonObject{{"status", "running"}});
        write(path("state/fugue/queue/0-00000000000001-20260101-000000-000-dead"),
              QJsonDocument(QJsonObject{{"run", "20260101-000000-000-dead"}, {"pid", double(dead)}}).toJson());
        const Outcome o = fugue({"run", path("flow.fugue"), "--wait"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(statusOf("20260101-000000-000-dead"), QString("failed"));
        QVERIFY(queued().isEmpty());
    }

    void validateWarnsWhenNoProviderCanServeANode() {
        write(path("config/fugue/providers.json"), "{\"providers\": [{\"name\": \"off\", \"type\": \"omarchy\", \"enabled\": false}]}");
        const Outcome o = fugue({"validate", path("gen.fugue"), "--json"});
        QCOMPARE(o.exitCode, 0); // a warning, not an error (8.5)
        const QJsonArray warnings = o.json().value("warnings").toArray();
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(warnings.at(0).toObject().value("code").toString(), QString("capability_unavailable"));
        QCOMPARE(warnings.at(0).toObject().value("severity").toString(), QString("warning"));
        QCOMPARE(warnings.at(0).toObject().value("node").toString(), QString("g"));
    }

    void aWorkerCollectsGarbageAtMostOncePerHour() {
        const auto oldRun = [&](const QString &id) {
            auto folder = RunFolder::create(runsRoot(), id);
            QVERIFY(folder.ok());
            folder->writeState(QJsonObject{{"status", "ok"}, {"ended", "2026-01-01T00:00:00.000"}});
        };
        oldRun("20260101-000000-000-old1");
        QCOMPARE(fugue({"run", path("flow.fugue"), "--wait"}).exitCode, 0);
        QVERIFY(!QDir(runsRoot() + "/20260101-000000-000-old1").exists());
        QVERIFY(QFile::exists(path("state/fugue/gc.stamp")));
        oldRun("20260101-000000-000-old2");
        QCOMPARE(fugue({"run", path("flow.fugue"), "--wait"}).exitCode, 0);
        QVERIFY(QDir(runsRoot() + "/20260101-000000-000-old2").exists()); // too soon to collect again
    }

    void theShippedBinaryHasNoKernelsYetSoARunFailsWithInternal() {
        const Outcome o = fugue({"run", path("flow.fugue"), "--wait", "--json"}, FUGUE_BINARY);
        QCOMPARE(o.exitCode, 1);
        const QList<QJsonObject> lines = o.lines();
        QCOMPARE(lines.last().value("event").toString(), QString("run.done"));
        QCOMPARE(lines.last().value("status").toString(), QString("failed"));
        QVERIFY(QString::fromUtf8(o.out).contains("\"code\":\"internal\""));
    }
};

QTEST_GUILESS_MAIN(TestCliRun)
#include "test_cli_run.moc"
