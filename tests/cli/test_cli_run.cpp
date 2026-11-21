// `fugue run` and what surrounds it (spec 9.8, 13.1): the queue folder and engine lock,
// the detached worker, --wait, cancel, --set and the garbage collection a worker does.
//
// Seam: the `fugue` command line as a subprocess, with no display. The binary is
// fugue_with_fakes: the same CLI, queue, lock and worker code as `fugue`, with fake node
// kernels and node types registered through cli::Hooks (the node types from the
// node.json files under $FUGUE_NODE_MANIFESTS, read by that binary's own main()). That
// is the more honest choice here. The production binary has no node types or kernels
// until M1, so through it nothing compiles; fake tables behind an env var would ship
// test code in the app. One test still drives the real `fugue` to pin that.
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
#include <sys/wait.h>
#include <unistd.h>

#include "engine/queue.h"
#include "cli/process.h"
#include "engine/registry.h"
#include "../engine/omarchy_fixture.h"
#include "cli/runs.h"
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
        // Not the developer's Omarchy state: its paired hosts would be instances (8.2).
        env.insert("HOME", path("home"));
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

    // Whether a worker of any run of this test is still a process (it exits just after it
    // removes its queue entry).
    bool anyWorkerAlive() const {
        const QStringList runs = QDir(runsRoot()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &pid : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile file("/proc/" + pid + "/cmdline");
            if (!pid.at(0).isDigit() || !file.open(QIODevice::ReadOnly))
                continue;
            const QList<QByteArray> arguments = file.readAll().split('\0');
            const qsizetype at = arguments.indexOf("__worker");
            if (at >= 0 && at + 1 < arguments.size() && runs.contains(QString::fromUtf8(arguments.at(at + 1))))
                return true;
        }
        return false;
    }

    // A process id that is certainly free: a child that has exited and been reaped.
    static qint64 deadPid() {
        const pid_t child = ::fork();
        if (child == 0)
            ::_exit(0);
        int status = 0;
        ::waitpid(child, &status, 0);
        return child;
    }

    void fabricateRun(const QString &id, const QString &status, const QString &entryClass, qint64 pid) const {
        auto folder = RunFolder::create(runsRoot(), id);
        QVERIFY(folder.ok());
        folder->writeState(QJsonObject{{"status", status}});
        write(queueDir() + "/" + entryClass + "-00000000000001-" + id,
              QJsonDocument(QJsonObject{{"run", id}, {"pid", double(pid)}}).toJson());
    }

    // For the parts of the CLI a test calls in-process.
    cli::Context context() const {
        cli::Context context;
        context.runsRoot = runsRoot();
        context.queueDir = queueDir();
        context.lockPath = path("state/fugue/engine.lock");
        return context;
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
        // The real condition, not a pause: the queue is empty and every worker has exited.
        QVERIFY2(QFile(gate()).open(QIODevice::WriteOnly), "gate");
        QTRY_VERIFY_WITH_TIMEOUT(queued().isEmpty() && !anyWorkerAlive(), 30000);
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
        // While a run waits, `fugue run` wrote its run.json once and nobody writes it again:
        // the worker's pid is in the queue entry, not patched into run.json.
        QVERIFY(!RunFolder::open(runsRoot(), b)->state().contains("pid"));
        QueueEntry entry;
        QVERIFY(Queue(queueDir()).find(b, &entry));
        QTRY_VERIFY_WITH_TIMEOUT(Queue(queueDir()).find(b, &entry) && isRunWorker(entry.pid, b), 10000);
        const Outcome o = fugue({"runs", "cancel", b, "--json"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(statusOf(b), QString("cancelled"));
        QCOMPARE(queued().size(), 1);
        QVERIFY(queued().first().endsWith(a));
        QVERIFY(QFile(gate()).open(QIODevice::WriteOnly));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(a), QString("ok"), 20000);
        QCOMPARE(statusOf(b), QString("cancelled")); // its worker did not run it
    }

    // 9.8: `runs cancel` reads the queue, then acts. The worker may activate the entry in
    // between; the cancel must then reach the worker, not delete the entry of a live run.
    void aCancelThatSawTheRunWaitingStillStopsItOnceItHasStarted() {
        const QString id = start("hold.fugue");
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(id), QString("running"), 15000);
        const Queue queue(queueDir());
        QueueEntry seen;
        QVERIFY(queue.find(id, &seen));
        QVERIFY(seen.active);
        // The entry as it was just before the worker renamed it to the active class.
        seen.active = false;
        seen.file = "5" + seen.file.mid(1);

        const cli::Reply reply = cli::cancelRun(*RunFolder::open(runsRoot(), id), seen, context());
        QVERIFY(!reply.failed());
        QCOMPARE(reply.data.toObject().value("status").toString(), QString("cancelling"));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(id), QString("cancelled"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(queued().isEmpty(), 10000); // the worker removed its own entry
        // The worker ended the run, once: it skipped the node and wrote the one run.done.
        int done = 0;
        bool nodeEnded = false;
        for (const QJsonObject &event : RunFolder::open(runsRoot(), id)->events()) {
            done += event.value("event").toString() == "run.done";
            nodeEnded = nodeEnded || (event.value("node").toString() == "h"
                                      && event.value("event").toString() == "node.skipped");
        }
        QCOMPARE(done, 1);
        QVERIFY(nodeEnded);
    }

    // A window that closed (or crashed) mid-run is not asked to cancel through its folder: no
    // one would read the request. Its run is ended as its worker's would be.
    void cancellingADeadWindowsRunSaysTheWorkerIsGone() {
        const QString id = "20260101-000000-000-wind";
        auto folder = RunFolder::create(runsRoot(), id);
        QVERIFY(folder.ok());
        folder->writeState(QJsonObject{{"status", "running"}});
        write(queueDir() + "/0-00000000000001-" + id,
              QJsonDocument(QJsonObject{{"run", id}, {"pid", double(deadPid())}, {"owner", "window"},
                                        {"process_start", 1}})
                  .toJson());
        const Outcome o = fugue({"runs", "cancel", id, "--json"});
        QVERIFY(o.exitCode != 0);
        QVERIFY(o.data().value("status").toString() != QLatin1String("cancelling"));
        QCOMPARE(statusOf(id), QString("failed"));
        QVERIFY(!QFile::exists(RunFolder::open(runsRoot(), id)->path() + "/cancel"));
        QVERIFY(queued().isEmpty());
    }

    void anEntryWhoseProcessIsGoneIsDiscarded() {
        // A crashed worker left itself first in the queue, and its run unfinished.
        const QString dead = "20260101-000000-000-dead";
        fabricateRun(dead, "running", "0", deadPid());
        const Outcome o = fugue({"run", path("flow.fugue"), "--wait"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(statusOf(dead), QString("failed"));
        // It has an end, so the run folder can be collected 7 days later (10.5).
        QVERIFY(!RunFolder::open(runsRoot(), dead)->state().value("ended").toString().isEmpty());
        QVERIFY(queued().isEmpty());
    }

    // A process id proves nothing: the dead worker's may belong to someone else by now.
    void aWaitingEntryWhoseWorkersPidWasReusedDoesNotBlockTheQueue() {
        const QString dead = "20260101-000000-000-gone";
        fabricateRun(dead, "queued", "3", QCoreApplication::applicationPid()); // alive, but not that worker
        const Outcome o = fugue({"run", path("flow.fugue"), "--wait"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(statusOf(dead), QString("failed"));
        QTRY_VERIFY(queued().isEmpty()); // the worker drops its own entry just after run.done (a CI run caught it first)
    }

    void followingARunWhoseWorkerWasKilledEndsInsteadOfHanging() {
        const QString id = start("hold.fugue");
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(id), QString("running"), 15000);
        const qint64 worker = qint64(RunFolder::open(runsRoot(), id)->state().value("pid").toDouble());
        QVERIFY(worker > 0);
        QCOMPARE(::kill(pid_t(worker), SIGKILL), 0);

        const Outcome o = fugue({"runs", "events", id, "--follow"});
        QCOMPARE(o.exitCode, 1);
        QCOMPARE(o.lines().last().value("event").toString(), QString("run.done"));
        QCOMPARE(o.lines().last().value("status").toString(), QString("failed"));
        QCOMPARE(statusOf(id), QString("failed"));
        QVERIFY(!RunFolder::open(runsRoot(), id)->state().value("ended").toString().isEmpty());
        QVERIFY(queued().isEmpty());
        QCOMPARE(fugue({"runs", "status", id, "--json"}).data().value("status").toString(), QString("failed"));
    }

    void cancelNeverSignalsAProcessThatIsNotTheRunsWorker() {
        // The active entry's pid now belongs to a bystander: `cat`, which runs until its
        // input closes.
        QProcess bystander;
        bystander.start("cat", QStringList());
        QVERIFY(bystander.waitForStarted());
        const QString id = "20260101-000000-000-reused";
        fabricateRun(id, "running", "0", bystander.processId());

        const Outcome o = fugue({"runs", "cancel", id, "--json"});
        QCOMPARE(o.exitCode, 1);
        QCOMPARE(o.firstCode(), QString("internal"));
        QCOMPARE(statusOf(id), QString("failed"));
        QVERIFY(queued().isEmpty());

        // A signal sent by the cancel would be delivered before the end of input is read.
        bystander.closeWriteChannel();
        QVERIFY(bystander.waitForFinished());
        QCOMPARE(bystander.exitStatus(), QProcess::NormalExit);
        QCOMPARE(bystander.exitCode(), 0);
    }

    // 4.8: "A compiled graph can be run directly."
    void aCompiledGraphRunsDirectly() {
        const Outcome compiled = fugue({"compile", path("flow.fugue"), "--json"});
        QCOMPARE(compiled.exitCode, 0);
        write(path("compiled.json"), QJsonDocument(compiled.data()).toJson());
        const Outcome o = fugue({"run", path("compiled.json"), "--wait"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(o.lines().last().value("status").toString(), QString("ok"));
        QJsonObject done;
        for (const QJsonObject &event : o.lines())
            if (event.value("event").toString() == "node.done" && event.value("node").toString() == "b")
                done = event;
        QCOMPARE(done.value("outputs").toObject().value("text").toString(), QString("hello"));

        // What changes a compile cannot be applied to a graph that is already compiled.
        const Outcome set = fugue({"run", path("compiled.json"), "--set", "b.text=x", "--json"});
        QCOMPARE(set.exitCode, 1);
        QCOMPARE(set.firstCode(), QString("usage"));
        // A compiled graph that is not one is refused with what is wrong.
        write(path("bad-compiled.json"), "{\"fugue-compiled\": 1, \"nodes\": 3}");
        const Outcome bad = fugue({"run", path("bad-compiled.json"), "--json"});
        QCOMPARE(bad.exitCode, 1);
        QCOMPARE(bad.firstCode(), QString("invalid_document"));
        QTRY_VERIFY_WITH_TIMEOUT(queued().isEmpty(), 10000);
    }

    // 1.4: XDG variables are honored.
    void providersJsonIsReadFromXdgConfigHome() {
        write(path("config/fugue/providers.json"),
              "{\"providers\": [{\"name\": \"Elsewhere\", \"type\": \"omarchy\", \"enabled\": false}]}");
        write(path("pinned.fugue"), flow("\"a\": {\"type\": \"fugue/note\", \"version\": \"1.0.0\", \"provider\": \"Elsewhere\"},"
                                         + node("b", "fugue/echo", "{\"text\": {\"from\": \"a.text\"}}")));
        const Outcome o = fugue({"compile", path("pinned.fugue"), "--json"});
        QVERIFY2(o.exitCode == 0, o.out.constData()); // not unknown_provider: the pin names a configured instance
    }

    // A provider instance that cannot be made is said, not dropped in silence.
    void validateWarnsAboutAProviderInstanceThatCannotBeMade() {
        write(path("config/fugue/providers.json"), "{\"providers\": [{\"name\": \"Cloud\", \"type\": \"no-such-module\"}]}");
        const Outcome o = fugue({"validate", path("flow.fugue"), "--json"});
        QCOMPARE(o.exitCode, 0);
        const QJsonArray warnings = o.json().value("warnings").toArray();
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(warnings.at(0).toObject().value("code").toString(), QString("unknown_provider"));
        QCOMPARE(warnings.at(0).toObject().value("severity").toString(), QString("warning"));
    }

    // The worker reads providers.json when its turn comes, which may be long after
    // `fugue run` compiled; a file broken in between fails the run with the reason.
    void aWorkerThatCannotReadProvidersJsonFailsTheRunWithTheReason() {
        const QString a = start("hold.fugue");
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(a), QString("running"), 15000);
        const QString b = start("flow.fugue");
        QVERIFY(!b.isEmpty());
        write(path("config/fugue/providers.json"), "{ this is not JSON");
        QVERIFY(QFile(gate()).open(QIODevice::WriteOnly));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(a), QString("ok"), 20000); // it had its providers already
        QTRY_VERIFY_WITH_TIMEOUT(statusOf(b) == "failed" || statusOf(b) == "ok", 20000);
        QCOMPARE(statusOf(b), QString("failed"));
        const QJsonObject done = RunFolder::open(runsRoot(), b)->events().last();
        QCOMPARE(done.value("event").toString(), QString("run.done"));
        QCOMPARE(done.value("errors").toArray().at(0).toObject().value("code").toString(), QString("invalid_json"));
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

    void withoutProvidersJsonValidateAsksThePairedHosts() {
        // The spec 8.2 default, from the Omarchy state in $HOME: this machine (text models
        // only), lab (offline) and studio, whose servers refuse here (port 1). The warning
        // names what each of them lacks.
        test::writeOmarchyState(path("home"), "http://127.0.0.1:1");
        const Outcome o = fugue({"validate", path("gen.fugue"), "--json"});
        QCOMPARE(o.exitCode, 0);
        const QJsonArray warnings = o.json().value("warnings").toArray();
        QCOMPARE(warnings.size(), 1);
        const QJsonObject warning = warnings.at(0).toObject();
        QCOMPARE(warning.value("code").toString(), QString("capability_unavailable"));
        QVERIFY2(warning.value("message").toString().contains("lab is offline."), qPrintable(warning.value("message").toString()));
        QVERIFY(warning.value("message").toString().contains("flux2-klein-4b-sdcpp-cuda is not answering"));
        QVERIFY(warning.value("hint").toString().contains("Turn on lab"));
    }

    void aWindowRunRegistersTheSameDefaultInstances() {
        // RunService's default (run_service.h) is cli::registerProviders, as for a worker.
        test::PairedHome home;
        Registry registry;
        const Errors problems = cli::registerProviders(registry);
        QVERIFY(!hasErrors(problems));
        QStringList names;
        for (const ProviderEntry &entry : registry.providers()) {
            names << entry.instance.name;
            QVERIFY(entry.provider);
        }
        QCOMPARE(names, (QStringList{"This machine", "lab", "studio"}));
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

    void theShippedBinaryDoesNotKnowTheTestFakes() {
        const Outcome o = fugue({"run", path("flow.fugue"), "--wait", "--json"}, FUGUE_BINARY);
        QCOMPARE(o.exitCode, 1);
        QCOMPARE(o.firstCode(), QString("unknown_node_type"));
        QVERIFY(queued().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCliRun)
#include "test_cli_run.moc"
