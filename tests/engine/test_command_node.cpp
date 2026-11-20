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
#include "engine/hash.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/resident_models.h"

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

// Running, not merely still in the process table: a process whose parent is gone is adopted by
// init, and in a container init may never reap it, so it stays there as a zombie (state Z).
bool alive(qint64 pid) {
    if (pid <= 0 || ::kill(pid_t(pid), 0) != 0)
        return false;
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly))
        return false;
    // Read while its zombie is being reaped, the file opens but reads empty: gone too.
    const QByteArray text = stat.readAll();
    const int close = text.lastIndexOf(')'); // "pid (name) S ..."; the name may hold spaces
    return close >= 0 && text.mid(close + 2, 1) != "Z";
}

// Once seen dead (a zombie, or reaped) it is dead: not looked at again, as /proc can say
// odd things about a pid while it is being reaped.
bool waitGone(qint64 pid, int ms = 3000) {
    QElapsedTimer clock;
    clock.start();
    for (;;) {
        if (!alive(pid))
            return true;
        if (clock.elapsed() >= ms)
            return false;
        QTest::qWait(20);
    }
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

Node makeNode(const QString &root, const QString &name, const QByteArray &script, const QJsonObject &runtimeExtra = {},
              const QJsonArray &extraInputs = {}) {
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

    QJsonArray inputs{QJsonObject{{"name", "mode"}, {"type", "text"}, {"optional", true}},
                      QJsonObject{{"name", "amount"}, {"type", "number"}, {"default", 1}}};
    for (const QJsonValue &input : extraInputs)
        inputs.append(input);
    QJsonObject runtime{{"kind", "command"}, {"entry", "./run.sh"}};
    for (auto it = runtimeExtra.begin(); it != runtimeExtra.end(); ++it)
        runtime.insert(it.key(), it.value());
    const QJsonObject manifest{
        {"manifest", 1},
        {"id", "io.github.test.tools/" + name},
        {"version", "1.2.0"},
        {"name", name},
        {"runtime", runtime},
        {"inputs", inputs},
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
    QStringList m_previews;

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
        context.preview = [this](const QString &file) { m_previews << file; };
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
        m_previews.clear();
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

    // --- resource inputs (7.2, 8.9) ---

    // A model file in Fugue's models folder (the test's own XDG data folder), declared as the
    // resource type io.github.test.tools/<type>, and a handle for it.
    QJsonObject modelHandle(const QString &typeName, const QString &file, const QByteArray &bytes) {
        const QString id = "io.github.test.tools/" + typeName;
        if (!m_registry.portType(*PortType::parse(id))) {
            DeclaredPortType declared;
            declared.type = *PortType::parse(id);
            declared.kind = PortKind::Resource;
            declared.name = typeName;
            declared.extensions = {".safetensors"};
            if (!m_registry.add(declared).isEmpty())
                qFatal("cannot declare %s", qPrintable(id));
        }
        const QString path = dataDir() + "/models/" + typeName + "/" + file;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            qFatal("cannot write %s", qPrintable(path));
        out.write(bytes);
        return QJsonObject{{"resource", id}, {"hash", sha256(bytes)}, {"name", file}};
    }

    void aResourceInputGetsThePathOfItsModelFile() {
        const QJsonObject base = modelHandle("base", "base.safetensors", "base weights");
        const QJsonObject lora1 = modelHandle("lora", "detail.safetensors", "lora one");
        const QJsonObject lora2 = modelHandle("lora", "style/ink.safetensors", "lora two");
        const QJsonObject derived{{"resource", "io.github.test.tools/base"},
                                  {"derived", QJsonObject{{"base", base},
                                                          {"apply", QJsonArray{QJsonObject{{"lora", lora1}, {"strength", 0.8}}}}}}};
        const Node node = makeNode(m_root.path(), "uses-models", "@echo.sh", {},
                                   QJsonArray{QJsonObject{{"name", "model"}, {"type", "io.github.test.tools/base"}},
                                              QJsonObject{{"name", "loras"}, {"type", "io.github.test.tools/lora"}, {"list", true}},
                                              QJsonObject{{"name", "merged"}, {"type", "io.github.test.tools/base"}, {"optional", true}}});
        CommandKernel kernel(kGraceMs);
        const NodeContext context = contextFor(node, {{"model", Value(base)},
                                                      {"loras", Value::list({Value(lora1), Value(lora2)})},
                                                      {"merged", Value(derived)}});
        const Result<Values> result = kernel.run(context);
        QVERIFY2(result.ok(), qPrintable(result.error().message));

        const QJsonObject inputs =
            QJsonDocument::fromJson(readFile(context.workDir + "/request.json")).object().value("inputs").toObject();
        const QString models = dataDir() + "/models/";
        QJsonObject expected = base;
        expected.insert("path", models + "base/base.safetensors");
        QCOMPARE(inputs.value("model").toObject(), expected); // the handle as it was, and its path
        const QJsonArray loras = inputs.value("loras").toArray();
        QCOMPARE(loras.size(), 2);
        QCOMPARE(loras.at(0).toObject().value("path").toString(), models + "lora/detail.safetensors");
        QCOMPARE(loras.at(1).toObject().value("path").toString(), models + "lora/style/ink.safetensors");
        // A derived handle has no file of its own: its base and what is applied to it get theirs.
        const QJsonObject merged = inputs.value("merged").toObject();
        QVERIFY(!merged.contains("path"));
        const QJsonObject parts = merged.value("derived").toObject();
        QCOMPARE(parts.value("base").toObject().value("path").toString(), models + "base/base.safetensors");
        QCOMPARE(parts.value("apply").toArray().at(0).toObject().value("lora").toObject().value("path").toString(),
                 models + "lora/detail.safetensors");
        QCOMPARE(parts.value("apply").toArray().at(0).toObject().value("strength").toDouble(), 0.8);
    }

    void aModelThatIsNotOnDiskFailsTheNodeWithModelMissing() {
        QJsonObject gone = modelHandle("base", "gone.safetensors", "gone weights");
        QVERIFY(QFile::remove(dataDir() + "/models/base/gone.safetensors"));
        const Node node = makeNode(m_root.path(), "needs-gone", "@echo.sh", {},
                                   QJsonArray{QJsonObject{{"name", "model"}, {"type", "io.github.test.tools/base"}}});
        CommandKernel kernel(kGraceMs);
        const NodeContext context = contextFor(node, {{"model", Value(gone)}});
        const Result<Values> result = kernel.run(context);
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, QString("model_missing"));
        QCOMPARE(result.error().input, QString("model"));
        QVERIFY2(result.error().message.contains("gone.safetensors"), qPrintable(result.error().message));
        QVERIFY(!QFileInfo::exists(context.workDir + "/request.json")); // the program never started
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

    // A `preview` line (7.2) shows an image the program wrote in its work_dir: a relative path
    // resolves against work_dir, as an output's does.
    void aPreviewLineShowsTheImageItNames() {
        const Node node = makeNode(m_root.path(), "steps", QByteArray("cat > /dev/null\necho ") + kPng +
                                                               " | base64 -d > step.png\n"
                                                               "echo '{\"op\": \"preview\", \"image\": {\"path\": \"step.png\"}}'\n"
                                                               "echo '{\"outputs\": {\"text\": \"done\"}}'\n"
                                   "echo '{\"op\": \"preview\", \"image\": {\"path\": \"step.png\"}}'\n");
        CommandKernel kernel(kGraceMs);
        const NodeContext context = contextFor(node);
        const Result<Values> result = kernel.run(context);
        QVERIFY2(result.ok(), qPrintable(result.error().message));
        // The preview after the result is not shown.
        QCOMPARE(m_previews, QStringList{QFileInfo(context.workDir + "/step.png").canonicalFilePath()});
        QVERIFY(result.warnings().isEmpty());
    }

    // A preview is only a picture: one that names no file, or a file outside work_dir, is left
    // out and the run goes on, with one warning that says why (the node's author sees it in
    // run.json and the CLI's run.done).
    void aBadPreviewLineIsLeftOutWithAWarning() {
        const Node node = makeNode(m_root.path(), "badsteps",
                                   "cat > /dev/null\n"
                                   "echo '{\"op\": \"preview\", \"image\": {\"path\": \"../secret.txt\"}}'\n"
                                   "echo '{\"op\": \"preview\", \"image\": {\"path\": \"missing.png\"}}'\n"
                                   "echo '{\"op\": \"preview\"}'\n"
                                   "echo '{\"outputs\": {\"text\": \"done\"}}'\n");
        CommandKernel kernel(kGraceMs);
        const Result<Values> result = kernel.run(contextFor(node));
        QVERIFY2(result.ok(), qPrintable(result.error().message));
        QCOMPARE(result->value("text").text(), QString("done"));
        QVERIFY(m_previews.isEmpty());
        QCOMPARE(result.warnings().size(), 1);
        QCOMPARE(result.warnings().first().code, QString("node_bad_preview"));
        QVERIFY2(result.warnings().first().message.contains("outside"), qPrintable(result.warnings().first().message));
    }

    // With its work_dir gone, a preview has no folder to be inside of: none is shown, not even
    // a file named by an absolute path.
    void aPreviewIsNotShownOnceTheWorkDirIsGone() {
        const Node node = makeNode(m_root.path(), "gone",
                                   "cat > /dev/null\nd=$PWD\ncd ..\nrm -rf \"$d\"\n"
                                   "echo \"{\\\"op\\\": \\\"preview\\\", \\\"image\\\": {\\\"path\\\": \\\"$PWD/secret.txt\\\"}}\"\n"
                                   "echo '{\"outputs\": {}}'\n");
        CommandKernel kernel(kGraceMs);
        kernel.run(contextFor(node));
        QVERIFY(m_previews.isEmpty());
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

    // The pack was updated and loaded again (#70): its node runs the new files, so the program
    // still running from the old ones is stopped and a new one started.
    void aPersistentNodeLoadedAgainWithNewFilesStartsANewProgram() {
        Node node = makeNode(m_root.path(), "persreload", persistentScript(), {{"mode", "persistent"}});
        {
            QFile manifest(node.dir + "/node.json"); // the seal covers it, so it must be there
            QVERIFY(manifest.open(QIODevice::WriteOnly));
            manifest.write("{}");
        }
        node.type.seal = nodeSeal(node.dir, "./run.sh");
        QVERIFY(!node.type.seal.isEmpty());
        CommandKernel kernel(kGraceMs);
        QVERIFY(kernel.run(contextFor(node)).ok());
        const qint64 old = pidsIn(node.dir + "/pids.txt").value(0);

        QFile script(node.dir + "/run.sh");
        QVERIFY(script.open(QIODevice::Append));
        script.write("# version 2\n");
        script.close();
        node.type.seal = nodeSeal(node.dir, "./run.sh"); // what the loader takes on a reload
        const Result<Values> next = kernel.run(contextFor(node));
        QVERIFY2(next.ok(), qPrintable(next.error().message));
        QCOMPARE(next->value("text").text(), QString("run 1")); // a new program counts from 1
        QCOMPARE(pidsIn(node.dir + "/pids.txt").size(), 2);
        QVERIFY(waitGone(old));
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

    // A persistent node's preview carries its run's id; one for another run is not shown. Its
    // relative path is against this run's work_dir, not the program's folder (the first run's).
    void aPersistentNodeShowsThePreviewsOfItsRun() {
        const Node node = makeNode(m_root.path(), "persview", persistentScript(), {{"mode", "persistent"}});
        CommandKernel kernel(kGraceMs);
        QVERIFY(kernel.run(contextFor(node)).ok()); // starts the program in the first work_dir
        const NodeContext context = contextFor(node, {{"mode", Value("preview")}});
        const Result<Values> result = kernel.run(context);
        QVERIFY2(result.ok(), qPrintable(result.error().message));
        QCOMPARE(m_previews, QStringList{QFileInfo(context.workDir + "/step.png").canonicalFilePath()});
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

    // 7.3, 8.10: a persistent program that uses the GPU holds memory between runs, so it is a
    // resident model (9.3); dropping it when memory is needed stops the program.
    void aPersistentGpuProgramIsStoppedWhenItsMemoryIsNeeded() {
        Node node = makeNode(m_root.path(), "persgpu", persistentScript(), {{"mode", "persistent"}});
        node.type.needs = QJsonObject{{"gpu", QJsonObject{{"vram_mb", 4000}}}};
        CommandKernel kernel(kGraceMs);
        ResidentModels models;
        NodeContext context = contextFor(node);
        context.models = &models;
        QVERIFY(kernel.run(context).ok());
        context.workDir = contextFor(node).workDir;
        QVERIFY(kernel.run(context).ok()); // a second run keeps it: it is the same program
        const QList<qint64> pids = pidsIn(node.dir + "/pids.txt");
        QCOMPARE(pids.size(), 1);
        QVERIFY(alive(pids.first()));
        QCOMPARE(models.keys(), QStringList{"program:io.github.test.tools/persgpu"});

        QCOMPARE(models.evictLeastRecentlyUsed(), QStringLiteral("program:io.github.test.tools/persgpu"));
        QVERIFY2(waitGone(pids.first()), "the program was not stopped");

        context.workDir = contextFor(node).workDir;
        const Result<Values> next = kernel.run(context);
        QVERIFY2(next.ok(), qPrintable(next.error().message));
        QCOMPARE(pidsIn(node.dir + "/pids.txt").size(), 2);
        QVERIFY(models.contains("program:io.github.test.tools/persgpu"));
    }

    void aPersistentProgramWithoutTheGpuIsNotAResidentModel() {
        const Node node = makeNode(m_root.path(), "perscpu", persistentScript(), {{"mode", "persistent"}});
        CommandKernel kernel(kGraceMs);
        ResidentModels models;
        NodeContext context = contextFor(node);
        context.models = &models;
        QVERIFY(kernel.run(context).ok());
        QVERIFY(models.keys().isEmpty());
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
