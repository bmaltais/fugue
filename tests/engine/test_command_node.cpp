// The command-node runner (spec 7): real programs (shell scripts) started by CommandKernel,
// one-shot and persistent, with progress, errors, cancel, timeouts and the idle stop.
//
// Seam: CommandKernel::run() with a NodeContext, as the scheduler calls it.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include <sys/types.h>

#include "engine/command_node.h"
#include "engine/file_hashes.h"
#include "engine/registry.h"

using namespace fugue;

namespace {

constexpr int kGraceMs = 300;

// A 1x1 PNG, for a node that writes an image.
const char *kPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool alive(qint64 pid) { return pid > 0 && ::kill(pid_t(pid), 0) == 0; }

bool waitGone(qint64 pid, int ms = 3000) {
    QElapsedTimer clock;
    clock.start();
    while (alive(pid) && clock.elapsed() < ms)
        QTest::qWait(20);
    return !alive(pid);
}

QList<qint64> pidsIn(const QString &path) {
    QList<qint64> pids;
    for (const QByteArray &line : readFile(path).split('\n'))
        if (!line.trimmed().isEmpty())
            pids.append(line.trimmed().toLongLong());
    return pids;
}

// A node folder under `root` holding `script` as run.sh, and the NodeType read from its
// manifest, the way the pack loader makes it.
struct Node {
    QString dir;
    NodeType type;
};

Node makeNode(const QString &root, const QString &name, const QByteArray &script, const QJsonObject &runtimeExtra = {}) {
    const QString dir = root + "/" + name;
    QDir().mkpath(dir);
    QByteArray body = script;
    if (script.startsWith('@')) {
        const QString source = QFINDTESTDATA(QStringLiteral("data/command/") + QString::fromUtf8(script.mid(1)));
        body = readFile(source);
        if (body.isEmpty())
            qFatal("missing test script %s", script.constData());
    }
    QFile file(dir + "/run.sh");
    if (!file.open(QIODevice::WriteOnly))
        qFatal("cannot write %s", qPrintable(dir));
    file.write("#!/bin/bash\n" + body);
    file.close();
    file.setPermissions(file.permissions() | QFileDevice::ExeUser);

    QJsonObject runtime{{"kind", "command"}, {"entry", "./run.sh"}};
    for (auto it = runtimeExtra.begin(); it != runtimeExtra.end(); ++it)
        runtime.insert(it.key(), it.value());
    const QJsonObject manifest{
        {"manifest", 1},
        {"id", "io.github.test.tools/" + name},
        {"version", "1.2.0"},
        {"name", name},
        {"runtime", runtime},
        {"inputs", QJsonArray{QJsonObject{{"name", "mode"}, {"type", "text"}, {"optional", true}},
                              QJsonObject{{"name", "amount"}, {"type", "number"}, {"default", 1}}}},
        {"outputs", QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}, {"optional", true}},
                               QJsonObject{{"name", "image"}, {"type", "image"}, {"optional", true}}}}};
    Result<NodeType> type = NodeType::fromJson(manifest);
    Node node;
    node.dir = dir;
    if (!type.ok())
        qFatal("bad test manifest for %s: %s", qPrintable(name), qPrintable(type.error().message));
    node.type = type.value();
    node.type.dir = dir;
    return node;
}

} // namespace

class TestCommandNode : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_root;
    std::unique_ptr<FileHashes> m_hashes;
    Registry m_registry;
    int m_counter = 0;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    QStringList m_progress;

    NodeContext contextFor(const Node &node, const Values &inputs = {}) {
        NodeContext context;
        context.node = "n1";
        context.run = "r1";
        context.type = &node.type;
        context.inputs = inputs;
        context.workDir = m_root.path() + "/work" + QString::number(++m_counter);
        QDir().mkpath(context.workDir);
        context.hashes = m_hashes.get();
        context.registry = &m_registry;
        const auto cancel = m_cancel;
        context.cancelled = [cancel] { return cancel->load(); };
        context.progress = [this](double value, const QString &, const QString &message) {
            m_progress << QStringLiteral("%1 %2").arg(value).arg(message);
        };
        return context;
    }

    // Raises the cancel flag `delayMs` after the program has written `marker`, from another
    // thread, and notes when (m_raisedAt), so a test does not depend on how fast it starts.
    std::thread cancelWhenFile(const QString &marker, int delayMs = 50) {
        const auto flag = m_cancel;
        std::atomic<qint64> *raisedAt = &m_raisedAt;
        return std::thread([flag, raisedAt, marker, delayMs] {
            QElapsedTimer waited;
            waited.start();
            while (!QFileInfo::exists(marker) && waited.elapsed() < 20000)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            raisedAt->store(QDateTime::currentMSecsSinceEpoch());
            flag->store(true);
        });
    }
    std::atomic<qint64> m_raisedAt{0};

private slots:
    void init() {
        QVERIFY(m_root.isValid());
        QFile secret(m_root.path() + "/secret.txt");
        QVERIFY(secret.open(QIODevice::WriteOnly));
        secret.write("secret");
        m_hashes = std::make_unique<FileHashes>(m_root.path() + "/hashes.json");
        m_cancel = std::make_shared<std::atomic<bool>>(false);
        m_progress.clear();
    }

    // --- one-shot ---

    void oneShotGetsTheRequestAndReturnsItsResult() {
        const Node node = makeNode(m_root.path(), "echo", "@echo.sh");
        CommandKernel kernel(kGraceMs);
        const NodeContext context = contextFor(node, {{"mode", Value("fast")}, {"amount", Value(1.5)}});
        const Result<Values> result = kernel.run(context);
        QVERIFY2(result.ok(), qPrintable(result.error().message));
        QCOMPARE(result->value("text").text(), QString("hello"));
        QCOMPARE(m_progress, QStringList{"0.4 pass 2 of 5"});

        const QJsonObject request = QJsonDocument::fromJson(readFile(context.workDir + "/request.json")).object();
        QCOMPARE(request.value("protocol").toInt(), 1);
        QCOMPARE(request.value("op").toString(), QString("run"));
        QCOMPARE(request.value("id").toString(), QString("r1:n1"));
        QCOMPARE(request.value("node").toObject().value("type").toString(), QString("io.github.test.tools/echo"));
        QCOMPARE(request.value("node").toObject().value("version").toString(), QString("1.2.0"));
        QCOMPARE(request.value("inputs").toObject().value("mode").toString(), QString("fast"));
        QCOMPARE(request.value("inputs").toObject().value("amount").toDouble(), 1.5);
        QCOMPARE(request.value("work_dir").toString(), context.workDir);

        const QString env = QString::fromUtf8(readFile(context.workDir + "/env.txt"));
        QVERIFY(env.contains("FUGUE_PROTOCOL=1"));
        QVERIFY(env.contains("FUGUE_NODE_DIR=" + QFileInfo(node.dir).canonicalFilePath()));
        QVERIFY(env.contains("FUGUE_WORK_DIR=" + context.workDir));
    }

    void anUnsetOptionalInputIsAbsentFromTheRequest() {
        const Node node = makeNode(m_root.path(), "absent", "cat > request.json\necho '{\"outputs\": {}}'\n");
        CommandKernel kernel(kGraceMs);
        const NodeContext context = contextFor(node, {{"amount", Value(2)}});
        QVERIFY(kernel.run(context).ok());
        const QJsonObject inputs =
            QJsonDocument::fromJson(readFile(context.workDir + "/request.json")).object().value("inputs").toObject();
        QVERIFY(!inputs.contains("mode"));
        QVERIFY(inputs.contains("amount"));
    }

    void anExplicitResultOpAndAnImageOutput() {
        const Node node = makeNode(m_root.path(), "paint", QByteArray("cat > /dev/null\necho ") + kPng +
                                                               " | base64 -d > out.png\n"
                                                               "echo '{\"op\": \"result\", \"outputs\": {\"image\": {\"path\": \"out.png\"}}}'\n");
        CommandKernel kernel(kGraceMs);
        const NodeContext context = contextFor(node);
        const Result<Values> result = kernel.run(context);
        QVERIFY2(result.ok(), qPrintable(result.error().message));
        const MediaHandle image = result->value("image").media();
        QCOMPARE(image.width(), 1);
        QCOMPARE(image.height(), 1);
        QVERIFY(image.hash().startsWith("sha256:"));
        QCOMPARE(image.path(), QFileInfo(context.workDir + "/out.png").canonicalFilePath());
    }

    void anOutputFileOutsideTheWorkDirIsRefused() {
        const Node node = makeNode(m_root.path(), "escape", "@escape-absolute.sh");
        CommandKernel kernel(kGraceMs);
        const Result<Values> absolute = kernel.run(contextFor(node));
        QVERIFY(!absolute.ok());
        QCOMPARE(absolute.error().code, QString("node_bad_output"));
        QCOMPARE(absolute.error().output, QString("image"));
        QVERIFY2(absolute.error().message.contains("outside"), qPrintable(absolute.error().message));

        const Node viaLink = makeNode(m_root.path(), "vialink", "@escape-link.sh");
        const Result<Values> linked = kernel.run(contextFor(viaLink));
        QVERIFY(!linked.ok());
        QCOMPARE(linked.error().code, QString("node_bad_output"));
        QVERIFY2(linked.error().message.contains("outside"), qPrintable(linked.error().message));
    }

    void anErrorLineFailsTheNodeWithItsMessage() {
        const Node node = makeNode(m_root.path(), "bad", "@error-line.sh");
        CommandKernel kernel(kGraceMs);
        const Result<Values> result = kernel.run(contextFor(node));
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, QString("node_failed"));
        QVERIFY(result.error().message.contains("image is smaller than the kernel"));
        QVERIFY(result.error().message.contains("bad_input"));
    }

    void aNonzeroExitReportsTheLastTwentyLinesOfStderr() {
        const Node node = makeNode(m_root.path(), "noisy", R"SH(
cat > /dev/null
for i in $(seq 1 30); do echo "log line $i" >&2; done
exit 3
)SH");
        CommandKernel kernel(kGraceMs);
        const Result<Values> result = kernel.run(contextFor(node));
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, QString("node_failed"));
        QVERIFY(result.error().message.contains("code 3"));
        QVERIFY(result.error().message.contains("log line 30"));
        QVERIFY(result.error().message.contains("log line 11"));
        QVERIFY(!result.error().message.contains("log line 10\n"));
    }

    void exitingWithoutAResultOrAfterOneIsAFailure() {
        CommandKernel kernel(kGraceMs);
        const Node none = makeNode(m_root.path(), "none", "cat > /dev/null\n");
        const Result<Values> noResult = kernel.run(contextFor(none));
        QVERIFY(!noResult.ok());
        QVERIFY(noResult.error().message.contains("without a result"));

        const Node late = makeNode(m_root.path(), "late", "cat > /dev/null\necho '{\"outputs\": {}}'\nexit 2\n");
        const Result<Values> afterResult = kernel.run(contextFor(late));
        QVERIFY(!afterResult.ok());
        QCOMPARE(afterResult.error().code, QString("node_failed"));
    }

    void stdoutThatIsNotJsonIsAProtocolFailure() {
        const Node node = makeNode(m_root.path(), "chatty", "cat > /dev/null\necho 'hello world'\nsleep 30\n");
        CommandKernel kernel(kGraceMs);
        QElapsedTimer clock;
        clock.start();
        const Result<Values> result = kernel.run(contextFor(node));
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, QString("node_failed"));
        QVERIFY(result.error().message.contains("not a JSON object"));
        QVERIFY2(clock.elapsed() < 5000, "the program was not stopped");
    }

    void aProgramThatCannotStartFails() {
        Node node = makeNode(m_root.path(), "noexec", "echo hi\n");
        QFile::setPermissions(node.dir + "/run.sh", QFileDevice::ReadUser | QFileDevice::WriteUser);
        CommandKernel kernel(kGraceMs);
        const Result<Values> result = kernel.run(contextFor(node));
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, QString("node_failed"));
    }

    void anEntryOutsideTheNodeFolderIsRefused() {
        Node node = makeNode(m_root.path(), "outside", "echo hi\n");
        node.type.runtime["entry"] = "../echo/run.sh";
        makeNode(m_root.path(), "echo", "echo hi\n");
        CommandKernel kernel(kGraceMs);
        const Result<Values> result = kernel.run(contextFor(node));
        QVERIFY(!result.ok());
        QVERIFY(result.error().message.contains("inside the node's folder"));

        // A symbolic link out of the folder is no better.
        QFile::link(m_root.path() + "/echo/run.sh", node.dir + "/linked.sh");
        node.type.runtime["entry"] = "./linked.sh";
        const Result<Values> linked = kernel.run(contextFor(node));
        QVERIFY(!linked.ok());
        QVERIFY2(linked.error().message.contains("inside the node's folder"), qPrintable(linked.error().message));
    }

    void cancelStopsAOneShotProgramAndWhatItStarted() {
        const Node node = makeNode(m_root.path(), "slow", R"SH(
cat > /dev/null
sleep 60 &
echo $! > child.pid
wait
)SH");
        CommandKernel kernel(kGraceMs);
        const NodeContext context = contextFor(node);
        std::thread cancel = cancelWhenFile(context.workDir + "/child.pid");
        QElapsedTimer clock;
        clock.start();
        const Result<Values> result = kernel.run(context);
        cancel.join();
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, QString("cancelled"));
        QVERIFY2(clock.elapsed() < 4000, "cancel took too long");
        const qint64 child = readFile(context.workDir + "/child.pid").trimmed().toLongLong();
        QVERIFY(child > 0);
        QVERIFY2(waitGone(child), "the program's own child was left running");
    }

    void aProgramThatIgnoresSigtermIsKilledAfterTheGracePeriod() {
        const Node node = makeNode(m_root.path(), "stubborn", "@stubborn.sh");
        CommandKernel kernel(kGraceMs);
        const NodeContext context = contextFor(node);
        std::thread cancel = cancelWhenFile(context.workDir + "/started");
        const Result<Values> result = kernel.run(context);
        const qint64 took = QDateTime::currentMSecsSinceEpoch() - m_raisedAt.load();
        cancel.join();
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, QString("cancelled"));
        QVERIFY2(took >= kGraceMs, "killed before the grace period was over");
        QElapsedTimer clock;
        clock.start();
        QVERIFY2(took < 4000, "was not killed");
    }

    void whatAProgramLeftRunningIsKilledWhenItExits() {
        const Node node = makeNode(m_root.path(), "leaver",
                                   "cat > /dev/null\n(trap '' TERM; exec sleep 60) &\necho $! > child.pid\n"
                                   "echo '{\"outputs\": {}}'\n");
        CommandKernel kernel(kGraceMs);
        const NodeContext context = contextFor(node);
        QVERIFY(kernel.run(context).ok());
        const qint64 child = readFile(context.workDir + "/child.pid").trimmed().toLongLong();
        QVERIFY(child > 0);
        QVERIFY2(waitGone(child), "a child outlived its program");
    }

    void theTimeoutEndsAOneShotRunWithNodeTimeout() {
        const Node node = makeNode(m_root.path(), "forever", "cat > /dev/null\nsleep 60\n", {{"timeout_s", 0.3}});
        CommandKernel kernel(kGraceMs);
        const Result<Values> result = kernel.run(contextFor(node));
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, QString("node_timeout"));
    }

    // --- persistent ---

    static QByteArray persistentScript() { return "@persistent.sh"; }

    void aPersistentNodeStartsOnceAndAnswersEachRun() {
        const Node node = makeNode(m_root.path(), "pers", persistentScript(), {{"mode", "persistent"}});
        CommandKernel kernel(kGraceMs);
        const Result<Values> first = kernel.run(contextFor(node));
        QVERIFY2(first.ok(), qPrintable(first.error().message));
        QCOMPARE(first->value("text").text(), QString("run 1"));
        const Result<Values> second = kernel.run(contextFor(node));
        QVERIFY2(second.ok(), qPrintable(second.error().message));
        QCOMPARE(second->value("text").text(), QString("run 2"));
        QCOMPARE(pidsIn(node.dir + "/pids.txt").size(), 1);
    }

    void aPersistentNodeReportsProgressForItsRun() {
        const Node node = makeNode(m_root.path(), "persprog", persistentScript(), {{"mode", "persistent"}});
        CommandKernel kernel(kGraceMs);
        std::thread cancel = cancelWhenFile(node.dir + "/slow.started"); // ends the slow run
        const Result<Values> result = kernel.run(contextFor(node, {{"mode", Value("slow")}}));
        cancel.join();
        QVERIFY(!result.ok());
        QCOMPARE(m_progress, QStringList{"0.5 working"});
    }

    void cancelAsksAPersistentNodeAndKeepsItRunning() {
        const Node node = makeNode(m_root.path(), "perscancel", persistentScript(), {{"mode", "persistent"}});
        CommandKernel kernel(kGraceMs);
        std::thread cancel = cancelWhenFile(node.dir + "/slow.started");
        const Result<Values> cancelled = kernel.run(contextFor(node, {{"mode", Value("slow")}}));
        cancel.join();
        QVERIFY(!cancelled.ok());
        QCOMPARE(cancelled.error().code, QString("cancelled"));

        m_cancel->store(false);
        const Result<Values> next = kernel.run(contextFor(node));
        QVERIFY2(next.ok(), qPrintable(next.error().message));
        QCOMPARE(next->value("text").text(), QString("run 2"));
        QCOMPARE(pidsIn(node.dir + "/pids.txt").size(), 1); // the same process
    }

    void aPersistentNodeThatIgnoresCancelIsKilledAndTheNextRunStartsANewOne() {
        const Node node = makeNode(m_root.path(), "persdeaf", persistentScript(), {{"mode", "persistent"}});
        CommandKernel kernel(kGraceMs);
        std::thread cancel = cancelWhenFile(node.dir + "/slow.started");
        const Result<Values> cancelled = kernel.run(contextFor(node, {{"mode", Value("deaf")}}));
        const qint64 took = QDateTime::currentMSecsSinceEpoch() - m_raisedAt.load();
        cancel.join();
        QVERIFY(!cancelled.ok());
        QCOMPARE(cancelled.error().code, QString("cancelled"));
        QVERIFY2(took >= kGraceMs, "killed before the grace period was over");
        QVERIFY(took < 4000);
        const QList<qint64> pids = pidsIn(node.dir + "/pids.txt");
        QCOMPARE(pids.size(), 1);
        QVERIFY(waitGone(pids.first()));

        m_cancel->store(false);
        const Result<Values> next = kernel.run(contextFor(node));
        QVERIFY2(next.ok(), qPrintable(next.error().message));
        QCOMPARE(next->value("text").text(), QString("run 1"));
        QCOMPARE(pidsIn(node.dir + "/pids.txt").size(), 2);
    }

    void aPersistentNodeThatDiesFailsTheRunAndTheNextOneRestartsIt() {
        const Node node = makeNode(m_root.path(), "persdie", persistentScript(), {{"mode", "persistent"}});
        CommandKernel kernel(kGraceMs);
        const Result<Values> died = kernel.run(contextFor(node, {{"mode", Value("die")}}));
        QVERIFY(!died.ok());
        QCOMPARE(died.error().code, QString("node_failed"));
        QVERIFY(died.error().message.contains("code 3"));
        const Result<Values> next = kernel.run(contextFor(node));
        QVERIFY2(next.ok(), qPrintable(next.error().message));
        QCOMPARE(next->value("text").text(), QString("run 1"));
        QCOMPARE(pidsIn(node.dir + "/pids.txt").size(), 2);
    }

    void aPersistentNodeIsStoppedAfterTheIdleTimeout() {
        const Node node =
            makeNode(m_root.path(), "persidle", persistentScript(), {{"mode", "persistent"}, {"idle_timeout_s", 0.3}});
        CommandKernel kernel(kGraceMs);
        QVERIFY(kernel.run(contextFor(node)).ok());
        const QList<qint64> pids = pidsIn(node.dir + "/pids.txt");
        QCOMPARE(pids.size(), 1);
        QVERIFY2(waitGone(pids.first()), "the idle process was not stopped");

        const Result<Values> next = kernel.run(contextFor(node));
        QVERIFY2(next.ok(), qPrintable(next.error().message));
        QCOMPARE(next->value("text").text(), QString("run 1"));
        QCOMPARE(pidsIn(node.dir + "/pids.txt").size(), 2);
    }

    void aBusyPersistentNodeIsNotStoppedForBeingIdle() {
        const Node node =
            makeNode(m_root.path(), "persbusy", persistentScript(), {{"mode", "persistent"}, {"idle_timeout_s", 0.2}});
        CommandKernel kernel(kGraceMs);
        QVERIFY(kernel.run(contextFor(node)).ok()); // arms the idle timer, then a run takes over
        QTest::qWait(50);
        std::thread cancel = cancelWhenFile(node.dir + "/slow.started", 800); // longer than the idle timeout
        const Result<Values> result = kernel.run(contextFor(node, {{"mode", Value("slow")}}));
        cancel.join();
        QCOMPARE(result.error().code, QString("cancelled")); // asked to stop, not stopped for idling
        QCOMPARE(pidsIn(node.dir + "/pids.txt").size(), 1);
        QVERIFY(alive(pidsIn(node.dir + "/pids.txt").first()));
    }

    void theTimeoutEndsAPersistentRunWithNodeTimeout() {
        const Node node =
            makeNode(m_root.path(), "perstimeout", persistentScript(), {{"mode", "persistent"}, {"timeout_s", 0.4}});
        CommandKernel kernel(kGraceMs);
        const Result<Values> result = kernel.run(contextFor(node, {{"mode", Value("slow")}}));
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, QString("node_timeout"));
    }

    void shuttingTheKernelDownStopsPersistentNodes() {
        const Node node = makeNode(m_root.path(), "persdown", persistentScript(), {{"mode", "persistent"}});
        qint64 pid = 0;
        {
            CommandKernel kernel(kGraceMs);
            QVERIFY(kernel.run(contextFor(node)).ok());
            pid = pidsIn(node.dir + "/pids.txt").first();
            QVERIFY(alive(pid));
        }
        QVERIFY2(waitGone(pid), "the persistent process outlived the kernel");
    }

    void aNodeChangedOnDiskSinceItWasLoadedIsNotRun() {
        Node node = makeNode(m_root.path(), "sealed", "cat > /dev/null\necho '{\"outputs\": {}}'\n");
        {
            QFile manifest(node.dir + "/node.json");
            QVERIFY(manifest.open(QIODevice::WriteOnly));
            manifest.write("{}");
        }
        node.type.seal = nodeSeal(node.dir, "./run.sh");
        QVERIFY(!node.type.seal.isEmpty());
        CommandKernel kernel(kGraceMs);
        QVERIFY(kernel.run(contextFor(node)).ok());

        QFile script(node.dir + "/run.sh");
        QVERIFY(script.open(QIODevice::Append));
        script.write("# edited\n");
        script.close();
        const Result<Values> changed = kernel.run(contextFor(node));
        QVERIFY(!changed.ok());
        QVERIFY2(changed.error().message.contains("changed on disk"), qPrintable(changed.error().message));
    }

    void shuttingDownEndsARunInFlightAndRefusesNewOnes() {
        const Node node = makeNode(m_root.path(), "persshut", persistentScript(), {{"mode", "persistent"}});
        CommandKernel kernel(kGraceMs);
        Result<Values> running = makeError("pending", "pending");
        std::thread run([&] { running = kernel.run(contextFor(node, {{"mode", Value("slow")}})); });
        QElapsedTimer waited;
        waited.start();
        while (!QFileInfo::exists(node.dir + "/slow.started") && waited.elapsed() < 10000)
            QTest::qWait(10);
        kernel.shutdown();
        run.join();
        QVERIFY(!running.ok());
        QVERIFY(waitGone(pidsIn(node.dir + "/pids.txt").first()));
        const Result<Values> after = kernel.run(contextFor(node));
        QVERIFY(!after.ok());
        QVERIFY(after.error().message.contains("shutting down"));
    }

    void aRunWaitingForTheSameNodeHearsACancel() {
        const Node node = makeNode(m_root.path(), "persqueue", persistentScript(), {{"mode", "persistent"}});
        CommandKernel kernel(kGraceMs);
        // The first run holds the program until it is cancelled with a flag of its own.
        auto firstCancel = std::make_shared<std::atomic<bool>>(false);
        NodeContext first = contextFor(node, {{"mode", Value("slow")}});
        first.cancelled = [firstCancel] { return firstCancel->load(); };
        std::thread holder([&] { kernel.run(first); });
        QElapsedTimer waited;
        waited.start();
        while (!QFileInfo::exists(node.dir + "/slow.started") && waited.elapsed() < 10000)
            QTest::qWait(10);

        std::thread cancel = cancelWhenFile(node.dir + "/slow.started");
        const Result<Values> queued = kernel.run(contextFor(node));
        cancel.join();
        QVERIFY(!queued.ok());
        QCOMPARE(queued.error().code, QString("cancelled"));
        firstCancel->store(true);
        holder.join();
    }

    // --- the seam in the scheduler's kernels ---

    void theKernelsServeACommandTypeWithTheCommandRunner() {
        const Node node = makeNode(m_root.path(), "viakernels", "cat > /dev/null\necho '{\"outputs\": {\"text\": \"x\"}}'\n");
        Kernels kernels;
        QVERIFY(!kernels.node(node.type));
        kernels.setCommandKernel(std::make_shared<CommandKernel>(kGraceMs));
        QVERIFY(kernels.node(node.type));

        NodeType builtin = node.type;
        builtin.runtime = QJsonObject{{"kind", "builtin"}};
        QVERIFY(!kernels.node(builtin));
    }
};

QTEST_GUILESS_MAIN(TestCommandNode)
#include "test_command_node.moc"
