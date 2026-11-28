// The sandbox of a pack's programs (spec 6.7.1, #731): the bwrap command line (pure), the denylist of `needs.files`, and
// real programs run through CommandKernel and a pack's setup, which a hostile pack tries to get out of. The runs need bwrap
// and user namespaces; without them the run tests skip (the fallback test needs neither).
//
// Seam: sandbox::build / parseNeeds, CommandKernel::run() on a node loaded from a pack folder, addPack() for the setup.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine/command_node.h"
#include "engine/file_hashes.h"
#include "engine/pack_loader.h"
#include "engine/pack_manager.h"
#include "engine/pack_secrets.h"
#include "engine/paths.h"
#include "engine/project.h"
#include "engine/registry.h"
#include "engine/run_folder.h"
#include "engine/sandbox.h"
#include "engine/trust_store.h"

using namespace fugue;

namespace {

void write(const QString &path, const QByteArray &bytes, bool executable = false) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        qFatal("cannot write %s", qPrintable(path));
    file.write(bytes);
    file.close();
    if (executable)
        file.setPermissions(file.permissions() | QFileDevice::ExeUser);
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool hasPair(const QStringList &args, const QString &flag, const QString &first, const QString &second = {}) {
    for (qsizetype i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag && args[i + 1] == first && (second.isEmpty() || (i + 2 < args.size() && args[i + 2] == second)))
            return true;
    return false;
}

// `flag N dest` (--ro-bind-fd, --bind-fd): bound by a descriptor of the command's, at `dest`.
bool boundFd(const sandbox::Command &command, const QString &flag, const QString &dest) {
    const QStringList &args = command.arguments;
    for (qsizetype i = 0; i + 2 < args.size(); ++i)
        if (args[i] == flag && args[i + 2] == dest && command.descriptors->numbers().contains(args[i + 1].toInt()))
            return true;
    return false;
}

// What the hostile program tries, one word per attempt: ALLOWED when the attempt worked.
QByteArray hostileScript() {
    return QByteArrayList{
        "#!/bin/bash",
        "cat > /dev/null",
        "r=\"\"",
        "check() { name=$1; shift; if \"$@\" > /dev/null 2>&1; then r=\"$r $name=ALLOWED\"; else r=\"$r $name=denied\"; fi; }",
        "check ssh cat '@SSH@'",
        "check secrets cat '@SECRETS@'",
        "check config ls '@CONFIG@'",
        "check outside bash -c \"echo x > '@OUTSIDE@/x'\"",
        "check packwrite bash -c \"echo x > $FUGUE_NODE_DIR/../planted\"",
        "check net bash -c 'exec 3<>/dev/tcp/127.0.0.1/@PORT@'",
        "check userns unshare -U true",
        "check home test \"$HOME\" = /tmp/home",
        "echo hello > \"$FUGUE_WORK_DIR/out.txt\"",
        "check work test -s \"$FUGUE_WORK_DIR/out.txt\"",
        "printf '{\"outputs\":{\"text\":\"%s\"}}\\n' \"$r\"",
        ""}.join('\n');
}


// A program in Python that does what its "mode" input says: "hostile" tries to get out, "ok" and "link" make an output image
// (the second as a link to a file of the test's home), "chain" reports the size of the image it was handed. As a one-shot it
// answers one request; as a persistent one it answers each line.
QByteArray probeScript(bool persistent) {
    QByteArrayList lines{
        "#!/usr/bin/env python3",
        "import sys, json, os, base64",
        "PNG = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==')",
        "def attempt(name, fn):",
        "    try:",
        "        fn()",
        "        return name + '=ALLOWED'",
        "    except Exception:",
        "        return name + '=denied'",
        "def handle(m):",
        "    mode = m.get('inputs', {}).get('mode', 'hostile')",
        "    work = m['work_dir']",
        "    if mode == 'chain':",
        "        path = m['inputs']['image']['path']",
        "        return {'text': 'size=%d' % os.path.getsize(path)}",
        "    if mode == 'ok':",
        "        open(os.path.join(work, 'out.png'), 'wb').write(PNG)",
        "        return {'image': {'path': 'out.png'}}",
        "    if mode == 'link':",
        "        os.symlink('@SSH@', os.path.join(work, 'out.png'))",
        "        return {'image': {'path': 'out.png'}}",
        "    def write(path):",
        "        open(path, 'w').write('x')",
        "    r = [attempt('runs', lambda: write('@RUNS@/r0/compiled.json')),",
        "         attempt('ssh', lambda: open('@SSH@').read()),",
        "         attempt('pack', lambda: write(os.environ['FUGUE_NODE_DIR'] + '/../planted')),",
        "         attempt('work', lambda: write(os.path.join(work, 'mine.txt')))]",
        "    return {'text': ' '.join(r)}",
    };
    if (persistent) {
        lines << "print(json.dumps({'op': 'ready'}), flush=True)"
              << "for line in sys.stdin:"
              << "    m = json.loads(line)"
              << "    if m.get('op') == 'shutdown':"
              << "        break"
              << "    if m.get('op') == 'run':"
              << "        print(json.dumps({'op': 'result', 'id': m['id'], 'outputs': handle(m)}), flush=True)";
    } else {
        lines << "m = json.loads(sys.stdin.readline())"
              << "print(json.dumps({'op': 'result', 'id': m['id'], 'outputs': handle(m)}), flush=True)";
    }
    lines << "";
    return lines.join('\n');
}

} // namespace

class TestSandbox : public QObject {
    Q_OBJECT

    QTemporaryDir m_root;
    std::unique_ptr<FileHashes> m_hashes;
    QTcpServer m_server;
    int m_counter = 0;
    Registry m_registry;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);

    QString home() const { return QDir::homePath(); }
    QString secretsFile() const { return configDir() + "/pack-secrets.json"; }

    // A pack folder with one one-shot command node `hostile`, loaded the way the app loads it (so it has its pack version).
    const NodeType *makePack(const QString &id, const QJsonObject &needs = {}, bool community = false, const QByteArray &script = {}, bool persistent = false) {
        const QString dir = m_root.path() + "/packs/" + id;
        QJsonObject pack{{"manifest", 1}, {"id", id}, {"version", "1.0.0"}, {"name", "Hostile"}, {"nodes", QJsonArray{"hostile"}}};
        if (community)
            pack["community"] = true;
        write(dir + "/pack.json", QJsonDocument(pack).toJson());
        QJsonObject node{{"manifest", 1}, {"id", id + "/hostile"}, {"version", "1.0.0"}, {"name", "Hostile"},
                         {"runtime", persistent ? QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}, {"mode", "persistent"}, {"idle_timeout_s", 30}}
                                                : QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                         {"inputs", QJsonArray{QJsonObject{{"name", "mode"}, {"type", "text"}, {"optional", true}},
                                               QJsonObject{{"name", "image"}, {"type", "image"}, {"optional", true}}}},
                         {"outputs", QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}, {"optional", true}},
                                                QJsonObject{{"name", "image"}, {"type", "image"}, {"optional", true}}}}};
        if (!needs.isEmpty())
            node["needs"] = needs;
        write(dir + "/hostile/node.json", QJsonDocument(node).toJson());
        QByteArray body = script.isEmpty() ? hostileScript() : script;
        body.replace("@SSH@", (home() + "/.ssh/id_test").toUtf8());
        body.replace("@SECRETS@", secretsFile().toUtf8());
        body.replace("@CONFIG@", configDir().toUtf8());
        body.replace("@OUTSIDE@", (m_root.path() + "/outside").toUtf8());
        body.replace("@PORT@", QByteArray::number(m_server.serverPort()));
        body.replace("@RUNS@", RunFolder::defaultRoot().toUtf8());
        write(dir + "/hostile/run.sh", body, true);
        const LoadedPack loaded = loadPack(m_registry, dir, PackOrigin::Disk, PackDigest::Leave);
        if (!loaded.enabled)
            qFatal("pack did not load: %s", qPrintable(loaded.errors.isEmpty() ? QString() : loaded.errors.first().message));
        return m_registry.nodeType(id + "/hostile");
    }

    NodeContext contextFor(const NodeType &type, const Values &inputs = {}) {
        NodeContext context;
        context.node = "n1";
        context.run = "r1";
        context.type = &type;
        context.inputs = inputs;
        context.workDir = m_root.path() + "/work" + QString::number(++m_counter);
        QDir().mkpath(context.workDir);
        context.hashes = m_hashes.get();
        context.registry = &m_registry;
        const auto cancel = m_cancel;
        context.cancelled = [cancel] { return cancel->load(); };
        return context;
    }

    // The words the hostile program reported, as a set.
    QStringList runHostile(const NodeType &type, QString *error = nullptr) {
        CommandKernel kernel(300);
        const NodeContext context = contextFor(type);
        const Result<Values> result = kernel.run(context);
        if (!result.ok()) {
            if (error)
                *error = result.error().code + ": " + result.error().message + " " + result.error().hint;
            return {};
        }
        return result->value("text").text().split(' ', Qt::SkipEmptyParts);
    }

private slots:
    void initTestCase() {
        QVERIFY(m_root.isValid());
        QVERIFY(m_server.listen(QHostAddress::LocalHost));
        m_hashes = std::make_unique<FileHashes>(m_root.path() + "/hashes.json");
        QDir().mkpath(m_root.path() + "/outside");
        // What a hostile pack would go for, in the test's own home (never the owner's).
        write(home() + "/.ssh/id_test", "PRIVATE KEY");
        write(RunFolder::defaultRoot() + "/r0/compiled.json", "another run");
        QVERIFY(PackSecretStore().set("io.github.other.pack", "token_1", "other-secret", "https://api.example.com").isEmpty());
    }
    void cleanup() { sandbox::setBwrapForTest(std::nullopt); }

    // --- the command line (pure) ---

    void theCommandLineIsAListOfArgumentsWithTheWholeProfile() {
        QDir().mkpath(m_root.path() + "/arg/pack/.venv");
        QDir().mkpath(m_root.path() + "/arg/work");
        write(m_root.path() + "/arg/pack/run.sh", "#!/bin/sh\n", true);
        sandbox::Profile profile;
        profile.packDir = m_root.path() + "/arg/pack";
        profile.workDir = m_root.path() + "/arg/work";
        profile.program = m_root.path() + "/arg/pack/run.sh";
        profile.environment = {"FUGUE_PROTOCOL=1"};
        const sandbox::Command command = sandbox::build(profile);
        const QStringList &args = command.arguments;
        QVERIFY2(command.refused.isEmpty(), qPrintable(command.refused));
        for (const char *flag : {"--unshare-all", "--unshare-user", "--disable-userns", "--new-session", "--die-with-parent", "--clearenv"})
            QVERIFY2(args.contains(flag), flag);
        QVERIFY(!args.contains("--share-net")); // no `network`: the network namespace stays empty
        // The folders are bound by descriptor, never by a path a program could swap for a link before bwrap reads it.
        QVERIFY(boundFd(command, "--ro-bind-fd", profile.packDir));
        QVERIFY(boundFd(command, "--ro-bind-fd", profile.packDir + "/.venv")); // its environment folder: read-only but in a setup
        QVERIFY(boundFd(command, "--bind-fd", profile.workDir));
        QVERIFY(!hasPair(args, "--ro-bind", profile.packDir, profile.packDir) && !hasPair(args, "--bind", profile.workDir, profile.workDir));
        QVERIFY(hasPair(args, "--setenv", "FUGUE_PROTOCOL", "1"));
        QVERIFY(hasPair(args, "--setenv", "HOME", "/tmp/home")); // no home
        QVERIFY(args.contains("--tmpfs"));
        for (const QString &secret : {home() + "/.ssh", home() + "/.config", configDir(), stateDir()})
            QVERIFY2(!args.join(' ').contains(secret), qPrintable(secret)); // no home: only uv's Python installs, which a venv links to
        QCOMPARE(args.mid(args.size() - 2), (QStringList{"--", profile.program}));

        profile.needs.network = true;
        QVERIFY(sandbox::build(profile).arguments.contains("--share-net"));
    }

    void aSetupGetsTheNetworkItsFolderAndTheUvCacheAndNothingElse() {
        QDir().mkpath(m_root.path() + "/setup/pack");
        sandbox::Profile profile;
        profile.kind = sandbox::Profile::Kind::Setup;
        profile.packDir = m_root.path() + "/setup/pack";
        profile.uvCache = m_root.path() + "/setup/uv-cache";
        profile.program = "/usr/bin/true";
        const QStringList args = sandbox::build(profile).arguments;
        QVERIFY(args.contains("--share-net"));
        write(profile.packDir + "/uv.lock", "lock");
        QDir().mkpath(profile.packDir + "/.venv");
        profile.uvCache = sandbox::uvCacheFor("io.example.pack");
        const sandbox::Command withLockCommand = sandbox::build(profile);
        const QStringList &withLock = withLockCommand.arguments;
        QVERIFY(boundFd(withLockCommand, "--ro-bind-fd", profile.packDir)); // the pack's own files are read-only
        QVERIFY(boundFd(withLockCommand, "--bind-fd", profile.packDir + "/.venv"));
        QVERIFY(!withLock.join(' ').contains("uv.lock")); // uv sync --frozen reads the lock, never writes it
        QVERIFY(!hasPair(withLock, "--bind", profile.packDir, profile.packDir));
        QVERIFY(hasPair(withLock, "--bind-try", profile.uvCache, profile.uvCache));
        QVERIFY2(profile.uvCache.startsWith(cacheDir() + "/uv/io.example.pack+"), qPrintable(profile.uvCache)); // Fugue's own cache, one per setup
        QVERIFY(sandbox::uvCacheFor("io.example.pack") != profile.uvCache);
        QVERIFY(!withLock.join(' ').contains(home() + "/.cache/uv"));
        QVERIFY(!args.contains("--chdir") || hasPair(args, "--chdir", profile.packDir));
        QVERIFY(!hasPair(args, "--bind", profile.packDir, profile.packDir));
        QVERIFY(!args.join(' ').contains(configDir()));
    }

    void aHandedLinkBindsItsTargetOnlyInsideAnAllowedRoot() {
        const QString models = m_root.path() + "/models";
        const QString elsewhere = m_root.path() + "/elsewhere";
        write(models + "/real.onnx", "m");
        write(elsewhere + "/secret.onnx", "s");
        write(home() + "/.ssh/handed", "k");
        QVERIFY(QFile::link(models + "/real.onnx", models + "/inside.onnx"));
        QVERIFY(QFile::link(elsewhere + "/secret.onnx", models + "/outside.onnx"));
        QVERIFY(QFile::link(home() + "/.ssh/handed", models + "/key.onnx"));
        QDir().mkpath(m_root.path() + "/arg2/pack");
        sandbox::Profile profile;
        profile.packDir = m_root.path() + "/arg2/pack";
        profile.program = "/usr/bin/true";
        profile.modelRoots = {models};
        QVERIFY(::mkfifo(QFile::encodeName(m_root.path() + "/handed.fifo").constData(), 0600) == 0);
        profile.handedFiles = {models + "/real.onnx", models + "/inside.onnx", models + "/outside.onnx", models + "/key.onnx",
                               home() + "/.ssh/handed", models + "/missing.onnx", m_root.path() + "/handed.fifo"};
        const sandbox::Command command = sandbox::build(profile);
        QVERIFY(boundFd(command, "--ro-bind-fd", models + "/real.onnx"));
        QVERIFY(hasPair(command.arguments, "--symlink", models + "/real.onnx", models + "/inside.onnx")); // the link points at the bound target
        QVERIFY(!command.arguments.join(' ').contains(elsewhere)); // a link out of the roots: left out
        QVERIFY(!command.arguments.join(' ').contains(".ssh")); // and a denied path never, linked or handed directly
        QCOMPARE(command.skipped.size(), 5); // the FIFO too: a socket, a FIFO or a device is never handed over
        QVERIFY(command.refused.isEmpty());

        // A model file must lie in a model folder, on what was opened: one outside refuses the run instead of being left out.
        profile.handedFiles.clear();
        profile.modelFiles = {models + "/real.onnx"};
        QVERIFY(sandbox::build(profile).refused.isEmpty());
        profile.modelFiles = {models + "/real.onnx", elsewhere + "/secret.onnx"};
        const sandbox::Command refused = sandbox::build(profile);
        QVERIFY2(refused.refused.contains("model folder"), qPrintable(refused.refused));
        QVERIFY(!refused.arguments.join(' ').contains(elsewhere));
    }

    // --- needs.files and the denylist ---

    void grantsAreAnAllowlist() {
        QDir().mkpath(home() + "/snap/firefox");
        const QStringList readBad{"~/snap", "~/snap/firefox", "/var", "/home", outputDir(), outputDir() + "/Out","~/.ssh", "~/.ssh/id_rsa", "~/.gnupg", "~/.config/fugue", "~/.local/share/uv/credentials", "~/.local/share/keyrings",
                                  "~", "/", "/etc", "~/Documents/../.ssh", "relative/path", "~/.cache/huggingface/token", "~/.git-credentials",
                                  "~/.claude", "~/.mozilla/firefox", "~/.config/chromium", "~/.bashrc", "~/.local/bin", "~/.config/systemd/user",
                                  "~/.config/hypr", "~/.local/share/applications", "~/.anything-with-a-dot/inside", "/tmp/tmux-1000", "/var/tmp",
                                  "/run/user/1000", "/dev", "/proc", "/sys", "/boot", "/root", configDir(), secretsFile(), stateDir() + "/trust.json",
                                  dataDir() + "/packs", cacheDir(), baseConfigDir(), baseDataDir() + "/projects"};
        for (const QString &bad : readBad)
            QVERIFY2(!sandbox::parseNeeds({{"files", QJsonObject{{"read", QJsonArray{bad}}}}}).ok(), qPrintable("read " + bad));
        // No write grant in v1, not even in the output folder: a program returns files as outputs.
        for (const QString &bad : {outputDir() + "/Out", QString("~/Documents/Out")}) {
            const Result<sandbox::Needs> write = sandbox::parseNeeds({{"files", QJsonObject{{"write", QJsonArray{bad}}}}});
            QVERIFY2(!write.ok() && write.error().message.contains("write grants are not supported yet"), qPrintable("write " + bad));
        }
        const Result<sandbox::Needs> good = sandbox::parseNeeds({{"network", true}, {"gpu", QJsonObject{{"api", "any"}}},
                                                                 {"files", QJsonObject{{"read", QJsonArray{"~/Documents/Scans"}}, {"write", QJsonArray{}}}}});
        QVERIFY2(good.ok(), qPrintable(good.error().message));
        QVERIFY(good->network && good->gpu);
        QCOMPARE(good->readPaths, QStringList{QFileInfo(home()).canonicalFilePath() + "/Documents/Scans"});
        QCOMPARE(sandbox::plainLines(*good, true),
                 (QStringList{"Can use the network", "Can reach services on this computer and its X11/Xwayland display", "Can use the GPU",
                              "Can read ~/Documents/Scans"}));
        QCOMPARE(sandbox::plainLines({}, true, true).first(), QString("Setup uses the network"));
        QVERIFY(sandbox::plainLines({}, false).first().startsWith("NOT sandboxed"));
        QVERIFY(sandbox::plainLines({}, true).first().startsWith("Runs in a sandbox"));
    }

    void theGpuDeviceNodesAreBoundOnlyForAGpuNode() {
        QDir().mkpath(m_root.path() + "/gpu/pack");
        sandbox::Profile profile;
        profile.packDir = m_root.path() + "/gpu/pack";
        profile.program = "/usr/bin/true";
        QVERIFY(!sandbox::build(profile).arguments.join(' ').contains("nvidia"));
        profile.needs.gpu = true;
        profile.needs.gpuApi = "cuda";
        profile.gpuNodes = {"/dev/nvidiactl", "/dev/nvidia0", "/dev/nvidia-uvm"};
        const QStringList args = sandbox::build(profile).arguments;
        for (const char *node : {"/dev/nvidiactl", "/dev/nvidia0", "/dev/nvidia-uvm"})
            QVERIFY2(hasPair(args, "--dev-bind-try", node, node), node);
        QVERIFY(hasPair(args, "--ro-bind-try", "/sys/module/nvidia", "/sys/module/nvidia"));
        QVERIFY(hasPair(args, "--ro-bind-try", "/sys/module/nvidia_uvm", "/sys/module/nvidia_uvm"));
        QVERIFY(!args.join(' ').contains("/dev/dri"));
        // Found under /dev: the driver's nodes by name, never nvidia-modeset or the caps.
        profile.gpuNodes.clear();
        const QString found = sandbox::build(profile).arguments.join(' ');
        QVERIFY(!found.contains("nvidia-modeset") && !found.contains("nvidia-caps"));
        // Vulkan: the render nodes only, never /dev/dri as a whole or a card node (which can drive the displays).
        profile.gpuNodes.clear();
        profile.needs.gpuApi = "vulkan";
        const QStringList vulkan = sandbox::build(profile).arguments;
        QVERIFY(!hasPair(vulkan, "--dev-bind-try", "/dev/dri", "/dev/dri"));
        QVERIFY(!vulkan.join(' ').contains("/dev/dri/card"));
        for (const QFileInfo &render : QDir("/dev/dri").entryInfoList({"renderD*"}, QDir::System | QDir::Files))
            QVERIFY2(hasPair(vulkan, "--dev-bind-try", render.filePath(), render.filePath()), qPrintable(render.filePath()));
    }

    void bwrapIsOnlyEverTheOneInUsrBin() {
        QVERIFY(sandbox::bwrapPath().isEmpty() || sandbox::bwrapPath() == "/usr/bin/bwrap");
    }

    void uvPythonFollowsXdgDataHome() {
        qputenv("XDG_DATA_HOME", "/somewhere/data");
        QCOMPARE(sandbox::uvPythonDir(), QString("/somewhere/data/uv/python"));
        qputenv("UV_PYTHON_INSTALL_DIR", "/own/python");
        QCOMPARE(sandbox::uvPythonDir(), QString("/own/python"));
        qunsetenv("UV_PYTHON_INSTALL_DIR");
    }

    void aNodeWithADeniedPathOrAWriteGrantDoesNotLoad() {
        QJsonObject manifest{{"manifest", 1}, {"id", "io.example.tools/bad"}, {"version", "1.0.0"}, {"name", "Bad"},
                             {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                             {"needs", QJsonObject{{"files", QJsonObject{{"read", QJsonArray{"~/.ssh"}}}}}}};
        const Result<NodeType> type = NodeType::fromJson(manifest);
        QVERIFY(!type.ok());
        QVERIFY2(type.error().message.contains(".ssh"), qPrintable(type.error().message));
        manifest["needs"] = QJsonObject{{"files", QJsonObject{{"write", QJsonArray{outputDir()}}}}};
        const Result<NodeType> writes = NodeType::fromJson(manifest);
        QVERIFY(!writes.ok());
        QVERIFY2(writes.error().message.contains("write grants are not supported yet; return files as outputs"), qPrintable(writes.error().message));
    }

    // --- real programs ---

    void aHostilePackCannotReadWriteOrConnectButItsOutputWorks() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        const NodeType *type = makePack("io.example.hostile");
        QString error;
        const QStringList words = runHostile(*type, &error);
        QVERIFY2(!words.isEmpty(), qPrintable(error));
        for (const char *word : {"ssh=denied", "secrets=denied", "config=denied", "outside=denied", "packwrite=denied", "net=denied",
                                    "userns=denied", "home=ALLOWED", "work=ALLOWED"})
            QVERIFY2(words.contains(word), qPrintable(QString(word) + " not in " + words.join(QLatin1Char(' '))));
        QVERIFY(!QFileInfo::exists(m_root.path() + "/outside/x"));
        QVERIFY(!QFileInfo::exists(m_root.path() + "/packs/io.example.hostile/planted"));
    }

    void aPackThatDeclaresTheNetworkCanConnectToALocalServer() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        const NodeType *type = makePack("io.example.networked", {{"network", true}});
        const QStringList words = runHostile(*type);
        QVERIFY2(words.contains("net=ALLOWED"), qPrintable(words.join(' ')));
        QVERIFY2(words.contains("ssh=denied") && words.contains("outside=denied"), qPrintable(words.join(' ')));
    }

    void declaredFilesAreVisibleAndNothingElse() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        write(home() + "/Documents/Scans/page.txt", "scan");
        write(home() + "/Documents/Private/other.txt", "private");
        const QByteArray script = "#!/bin/bash\ncat > /dev/null\nr=\"\"\n"
                                  "check() { name=$1; shift; if \"$@\" > /dev/null 2>&1; then r=\"$r $name=ALLOWED\"; else r=\"$r $name=denied\"; fi; }\n"
                                  "check scan cat \"$HOME_REAL/Documents/Scans/page.txt\"\n"
                                  "check scanwrite bash -c \"echo x > '$HOME_REAL/Documents/Scans/new.txt'\"\n"
                                  "check other cat \"$HOME_REAL/Documents/Private/other.txt\"\n"
                                  "printf '{\"outputs\":{\"text\":\"%s\"}}\\n' \"$r\"\n";
        QByteArray body = script;
        body.replace("$HOME_REAL", QFileInfo(home()).canonicalFilePath().toUtf8());
        const NodeType *type = makePack("io.example.files", {{"files", QJsonObject{{"read", QJsonArray{"~/Documents/Scans"}}}}}, false, body);
        const QStringList words = runHostile(*type);
        QCOMPARE(words, (QStringList{"scan=ALLOWED", "scanwrite=denied", "other=denied"}));
    }

    void setupRunsInTheSandboxWithTheNetworkAndWritesOnlyItsOwnFolder() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        const QString source = m_root.path() + "/setup-source/io.example.setup";
        const QString command = QStringLiteral("echo made > .venv/made; echo x > evil 2>/dev/null; echo x > node.json.new 2>/dev/null; "
                                               "echo x > '%1/outside/from-setup' 2>/dev/null; "
                                               "cat '%2' > .venv/seen-ssh 2>/dev/null; exec 3<>/dev/tcp/127.0.0.1/%3 && echo net > .venv/net-ok; true")
                                    .arg(m_root.path(), home() + "/.ssh/id_test").arg(m_server.serverPort());
        write(source + "/pack.json",
              QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.setup"}, {"version", "1.0.0"}, {"name", "Setup"},
                                        {"nodes", QJsonArray{"tool"}},
                                        {"setup", QJsonObject{{"command", QJsonArray{"bash", "-c", command}}}}}).toJson());
        write(source + "/tool/node.json",
              QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.setup/tool"}, {"version", "1.0.0"}, {"name", "Tool"},
                                        {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}}}).toJson());
        write(source + "/tool/run.sh", "#!/bin/sh\n", true);
        TrustStore trust(m_root.path() + "/trust-setup.json");
        PackPrompts prompts;
        prompts.install = prompts.trust = prompts.runSetup = [](const PackSummary &) { return Answer::Yes; };
        const QString packs = m_root.path() + "/setup-packs";
        const Result<PackAdded> added = addPack(source, {}, packs, trust, prompts);
        QVERIFY2(added.ok(), qPrintable(added.error().message + " " + added.error().hint));
        QVERIFY(added->setupRan);
        QCOMPARE(readAll(packs + "/io.example.setup/.venv/made"), QByteArray("made\n")); // its environment folder is writable
        QVERIFY(QFileInfo::exists(packs + "/io.example.setup/.venv/net-ok"));             // the network is on
        QVERIFY(!QFileInfo::exists(packs + "/io.example.setup/evil"));                    // the rest of the pack is not (also its node.json)
        QVERIFY(!QFileInfo::exists(packs + "/io.example.setup/node.json.new"));
        QVERIFY(!QFileInfo::exists(m_root.path() + "/outside/from-setup"));               // nor anything else
        QVERIFY(readAll(packs + "/io.example.setup/.venv/seen-ssh").isEmpty());           // no home
    }


    // --- the escapes the review found (#731) ---

    static QString made(const Result<Values> &result) { return result->value("image").media().path(); }

    Result<Values> runProbe(CommandKernel &kernel, const NodeType &type, const QString &mode, const Values &more = {}, QString *workDir = nullptr) {
        Values inputs = more;
        inputs.insert("mode", Value(mode));
        const NodeContext context = contextFor(type, inputs);
        if (workDir)
            *workDir = context.workDir;
        return kernel.run(context);
    }

    // A program that lives on works in an exchange folder: it cannot write another run's records, and what it makes is copied out.
    void aPersistentProgramCannotTouchTheRunsFolderAndItsOutputsAreCopiedOut_data() {
        QTest::addColumn<bool>("persistent");
        QTest::newRow("one-shot") << false;
        QTest::newRow("persistent") << true;
    }
    void aPersistentProgramCannotTouchTheRunsFolderAndItsOutputsAreCopiedOut() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        QFETCH(bool, persistent);
        const QString id = persistent ? "io.example.livingprobe" : "io.example.oneshotprobe";
        makePack(id, {}, false, probeScript(persistent), persistent);
        const NodeType *type = m_registry.nodeType(id + "/hostile");
        CommandKernel kernel(300);
        const Result<Values> hostile = runProbe(kernel, *type, "hostile");
        QVERIFY2(hostile.ok(), qPrintable(hostile.error().message));
        QCOMPARE(hostile->value("text").text(), QString("runs=denied ssh=denied pack=denied work=ALLOWED"));
        QCOMPARE(readAll(RunFolder::defaultRoot() + "/r0/compiled.json"), QByteArray("another run"));

        QString workDir;
        const Result<Values> made = runProbe(kernel, *type, "ok", {}, &workDir); // an output image, copied out of the exchange folder
        QVERIFY2(made.ok(), qPrintable(made.error().message));
        QVERIFY(made->value("image").isMedia());
        QVERIFY2(made->value("image").media().path().startsWith(QFileInfo(workDir).canonicalFilePath() + "/"), qPrintable(made->value("image").media().path()));
        QVERIFY(QFileInfo(made->value("image").media().path()).isFile());

        // An output that is a link to a file of the home folder is refused, never read through.
        const Result<Values> linked = runProbe(kernel, *type, "link");
        QVERIFY(!linked.ok());
        QVERIFY2(linked.error().code == "node_bad_output", qPrintable(linked.error().code + linked.error().message));
        QCOMPARE(readAll(home() + "/.ssh/id_test"), QByteArray("PRIVATE KEY"));
    }

    // A node handed what an earlier pack node made: bound read-only (one-shot) or copied into the exchange folder (persistent).
    void chainedPackNodesPassTheirFiles_data() {
        QTest::addColumn<bool>("persistent");
        QTest::newRow("one-shot") << false;
        QTest::newRow("persistent") << true;
    }
    void chainedPackNodesPassTheirFiles() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        QFETCH(bool, persistent);
        const QString first = persistent ? "io.example.chainfirstp" : "io.example.chainfirst";
        const QString second = persistent ? "io.example.chainsecondp" : "io.example.chainsecond";
        makePack(first, {}, false, probeScript(false));
        makePack(second, {}, false, probeScript(persistent), persistent);
        CommandKernel kernel(300);
        const Result<Values> made = runProbe(kernel, *m_registry.nodeType(first + "/hostile"), "ok");
        QVERIFY2(made.ok(), qPrintable(made.error().message));
        const Result<Values> read = runProbe(kernel, *m_registry.nodeType(second + "/hostile"), "chain", {{"image", made->value("image")}});
        QVERIFY2(read.ok(), qPrintable(read.error().message));
        QCOMPARE(read->value("text").text(), QString("size=%1").arg(QFileInfo(made->value("image").media().path()).size()));
    }

    void theFolderThatIsBoundIsTheOneTheProgramIsToldEvenThroughALink() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        makePack("io.example.linkedwork");
        const NodeType *type = m_registry.nodeType("io.example.linkedwork/hostile");
        NodeContext context = contextFor(*type);
        const QString link = m_root.path() + "/work-link";
        QVERIFY(QFile::link(context.workDir, link));
        context.workDir = link;
        CommandKernel kernel(300);
        const Result<Values> result = kernel.run(context);
        QVERIFY2(result.ok(), qPrintable(result.error().message));
        QVERIFY2(result->value("text").text().contains("work=ALLOWED"), qPrintable(result->value("text").text()));
    }

    // Setup leaves what the person trusts: .venv stays a folder, nothing else of the pack changes, the marker is never written
    // through a link, and a pack edited to ask for more asks again.
    void aEnvironmentFolderThatIsALinkIsNeverBoundAndASetupThatMakesOneFails() {
        const QString victim = m_root.path() + "/victim-venv";
        QDir().mkpath(victim);
        QDir().mkpath(m_root.path() + "/arg3/pack");
        QVERIFY(QFile::link(victim, m_root.path() + "/arg3/pack/.venv"));
        sandbox::Profile profile;
        profile.packDir = m_root.path() + "/arg3/pack";
        profile.program = "/usr/bin/true";
        QVERIFY(!sandbox::build(profile).arguments.join(' ').contains(victim)); // a run: not bound
        profile.kind = sandbox::Profile::Kind::Setup;
        QVERIFY(!sandbox::build(profile).arguments.join(' ').contains(victim)); // nor in a setup
    }

    void aPackThatShipsALinkAsItsEnvironmentOrItsMarkerChangesNothingOutsideItself() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        const QString victimFolder = m_root.path() + "/ship-victim";
        const QString victimFile = victimFolder + "/bashrc";
        write(victimFile, "mine");
        const QString source = m_root.path() + "/ship-source/io.example.ship";
        write(source + "/pack.json",
              QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.ship"}, {"version", "1.0.0"}, {"name", "Ship"},
                                        {"nodes", QJsonArray{"tool"}}, {"setup", QJsonObject{{"command", QJsonArray{"bash", "-c", "echo pwned > .venv/x; true"}}}}}).toJson());
        write(source + "/tool/node.json",
              QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.ship/tool"}, {"version", "1.0.0"}, {"name", "Tool"},
                                        {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}}}).toJson());
        write(source + "/tool/run.sh", "#!/bin/sh\n", true);
        QVERIFY(QFile::link(victimFolder, source + "/.venv"));
        QVERIFY(QFile::link(victimFile, source + "/.fugue-setup.json"));
        TrustStore trust(m_root.path() + "/trust-ship.json");
        PackPrompts prompts;
        prompts.install = prompts.trust = prompts.runSetup = [](const PackSummary &) { return Answer::Yes; };
        const Result<PackAdded> added = addPack(source, {}, m_root.path() + "/ship-packs", trust, prompts);
        // Whether the install is refused or goes through without the links, nothing outside the pack was written.
        QCOMPARE(readAll(victimFile), QByteArray("mine"));
        QVERIFY(!QFileInfo::exists(victimFolder + "/x"));
        if (added.ok()) {
            QVERIFY(!QFileInfo(m_root.path() + "/ship-packs/io.example.ship/.venv").isSymLink());
        } else {
            QVERIFY2(added.error().code == "setup_failed" || added.error().code == "pack_invalid" || added.error().code == "permission_denied", qPrintable(added.error().code));
        }
    }

    void aPackEditedToAskForMoreAsksForTrustAgain() {
        const QString id = "io.example.edited";
        const NodeType *type = makePack(id);
        Q_UNUSED(type);
        const QString folder = m_root.path() + "/packs/" + id;
        TrustStore trust(m_root.path() + "/trust-edited.json");
        Registry first;
        QVERIFY(loadPack(first, folder, PackOrigin::Disk, PackDigest::Record).enabled);
        QVERIFY(trust.trust(id, "1.0.0").isEmpty());
        QVERIFY(trust.isTrusted(id, "1.0.0"));
        // The node.json is edited in place: more is asked for, so the yes no longer stands once the pack is loaded again.
        QJsonObject node = QJsonDocument::fromJson(readAll(folder + "/hostile/node.json")).object();
        node["needs"] = QJsonObject{{"network", true}};
        write(folder + "/hostile/node.json", QJsonDocument(node).toJson());
        Registry second;
        QVERIFY(loadPack(second, folder, PackOrigin::Disk, PackDigest::Record).enabled);
        QVERIFY(!trust.isTrusted(id, "1.0.0"));
    }

    // Fugue's descriptors and environment never reach the sandbox: not a directory descriptor another thread holds open
    // without close-on-exec (a program could openat() its way out of one), nor a variable of Fugue's in bwrap's own process,
    // whose /proc/1/environ the program can read.
    void noDescriptorOrVariableOfFuguesReachesTheProgram() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        qputenv("FUGUE_LEAK_MARKER", "leak-123");
        const int leaked = ::open(QFile::encodeName(m_root.path() + "/outside").constData(), O_RDONLY | O_DIRECTORY); // no O_CLOEXEC
        QVERIFY(leaked >= 0);
        std::atomic<bool> stop{false};
        std::thread holder([&] { // and one opened and closed over and over meanwhile
            while (!stop.load()) {
                const int fd = ::open(QFile::encodeName(m_root.path() + "/outside").constData(), O_RDONLY | O_DIRECTORY);
                if (fd >= 0)
                    ::close(fd);
            }
        });
        const QByteArray script = "#!/bin/bash\ncat > /dev/null\n"
                                  "fds=$(for f in /proc/self/fd/*; do readlink \"$f\"; done | tr '\\n' ' ')\n"
                                  "marker=$(tr '\\0' '\\n' < /proc/1/environ | grep -c FUGUE_LEAK_MARKER)\n"
                                  "printf '{\"outputs\":{\"text\":\"%s|%s\"}}\\n' \"$marker\" \"$fds\"\n";
        const NodeType *type = makePack("io.example.leaks", {}, false, script);
        CommandKernel kernel(300);
        const Result<Values> result = kernel.run(contextFor(*type));
        stop = true;
        holder.join();
        ::close(leaked);
        qunsetenv("FUGUE_LEAK_MARKER");
        QVERIFY2(result.ok(), qPrintable(result.error().message));
        const QStringList parts = result->value("text").text().split('|');
        QCOMPARE(parts.value(0), QString("0")); // bwrap's own environment holds none of Fugue's variables
        for (const QString &target : parts.value(1).split(' ', Qt::SkipEmptyParts))
            QVERIFY2(target.startsWith("pipe:") || target.startsWith("/dev/") || target.endsWith("/run.sh"), qPrintable(parts.value(1)));
    }

    // What a program prints to stderr reaches an error message without its escape sequences.
    void aProgramsEscapeSequencesNeverReachAMessage() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        const NodeType *type = makePack("io.example.escapes", {}, false, "#!/bin/bash\ncat > /dev/null\nprintf '\\033]0;owned\\007\\033[2Jboom\\n' >&2\nexit 3\n");
        CommandKernel kernel(300);
        const Result<Values> result = kernel.run(contextFor(*type));
        QVERIFY(!result.ok());
        QVERIFY2(result.error().message.contains("boom"), qPrintable(result.error().message));
        QVERIFY(!result.error().message.contains(QChar(0x1b)) && !result.error().message.contains(QChar(0x07)));
    }

    // A derived handle hands over the paths withPaths resolved (its base, its other parts, its LoRAs), each bound by descriptor;
    // a "path" a workflow wrote anywhere else in the value is nothing to the sandbox.
    void derivedModelHandlesHandOverExactlyWhatWasResolved() {
        const QString models = QFileInfo(m_root.path()).canonicalFilePath() + "/derived-models";
        for (const char *name : {"base.safetensors", "vae.safetensors", "lora.safetensors", "plain.onnx"})
            write(models + "/" + name, "m");
        const QJsonObject derived{
            {"resource", "fugue/model"},
            {"path", home() + "/.ssh/id_test"},
            {"derived", QJsonObject{{"base", QJsonObject{{"resource", "fugue/diffusion-model"}, {"path", models + "/base.safetensors"}}},
                                    {"vae", QJsonObject{{"resource", "fugue/vae"}, {"path", models + "/vae.safetensors"}}},
                                    {"note", QJsonObject{{"path", "/etc/shadow"}}},
                                    {"apply", QJsonArray{QJsonObject{{"lora", QJsonObject{{"resource", "fugue/lora"}, {"path", models + "/lora.safetensors"}}},
                                                                     {"weight", 1}}}}}}};
        const QJsonObject plain{{"resource", "fugue/model"}, {"path", models + "/plain.onnx"}};
        const QStringList paths = resolvedModelPaths(QJsonArray{derived, plain});
        QCOMPARE(paths, (QStringList{models + "/base.safetensors", models + "/vae.safetensors", models + "/lora.safetensors", models + "/plain.onnx"}));
        QDir().mkpath(m_root.path() + "/arg4/pack");
        sandbox::Profile profile;
        profile.packDir = m_root.path() + "/arg4/pack";
        profile.program = "/usr/bin/true";
        profile.modelRoots = {models};
        profile.modelFiles = paths;
        const sandbox::Command command = sandbox::build(profile);
        QVERIFY2(command.refused.isEmpty(), qPrintable(command.refused));
        for (const QString &path : paths)
            QVERIFY2(boundFd(command, "--ro-bind-fd", path), qPrintable(path));
        QVERIFY(!command.arguments.join(' ').contains(".ssh") && !command.arguments.join(' ').contains("/etc/shadow"));
    }

    // A program that lives on plants links in its exchange folder: where Fugue's next request folder would have been (it is
    // 128 random bits now), and its own `work` turned into a link to a folder of the test's. Nothing outside is created,
    // deleted or copied out, and the next request still works.
    void aProgramThatLivesOnCannotLeadTheExchangeFolderOutWithLinks() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        const QString victim = m_root.path() + "/outside/victim";
        write(victim + "/secret.png", "SECRET");
        QByteArray script = probeScript(true);
        script.replace("    if mode == 'ok':",
                       "    if mode == 'escape':\n"
                       "        request = os.path.dirname(work)\n"
                       "        root = os.path.dirname(request)\n"
                       "        for name in [str(n) for n in range(1, 40)] + ['next', 'in', 'work']:\n"
                       "            try: os.symlink('@VICTIM@', os.path.join(root, name))\n"
                       "            except OSError: pass\n"
                       "        os.rename(work, work + '.real')\n"
                       "        os.symlink('@VICTIM@', work)\n"
                       "        os.symlink('@VICTIM@', os.path.join(request, 'in', 'planted'))\n"
                       "        return {'image': {'path': 'secret.png'}}\n"
                       "    if mode == 'ok':");
        script.replace("@VICTIM@", victim.toUtf8());
        makePack("io.example.exchangelinks", {}, false, script, true);
        const NodeType *type = m_registry.nodeType("io.example.exchangelinks/hostile");
        QString escapeWork;
        {
            CommandKernel kernel(300);
            const Result<Values> escaped = runProbe(kernel, *type, "escape", {}, &escapeWork);
            QVERIFY(!escaped.ok());
            QVERIFY2(escaped.error().code == "node_bad_output", qPrintable(escaped.error().code + " " + escaped.error().message));
            QString okWork;
            const Result<Values> after = runProbe(kernel, *type, "ok", {}, &okWork);
            QVERIFY2(after.ok(), qPrintable(after.error().message));
            QVERIFY(made(after).startsWith(QFileInfo(okWork).canonicalFilePath() + "/"));
        } // the kernel goes: its program stops and its exchange folder is removed, links and all
        QCOMPARE(QDir(victim).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList{"secret.png"});
        QCOMPARE(readAll(victim + "/secret.png"), QByteArray("SECRET"));
        QVERIFY(QDir(escapeWork).entryList(QDir::Files).isEmpty()); // nothing was copied out of it
        QDirIterator left(cacheDir() + "/exchange", QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
        QVERIFY2(!left.hasNext(), qPrintable(left.hasNext() ? left.next() : QString()));
    }

    // A read grant swapped for a link between the check and the start: bwrap binds the descriptor that was checked, not
    // what the path names a moment later; and the next start refuses the link.
    void aGrantSwappedForALinkAfterTheCheckBindsWhatWasChecked() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        const QString grant = QFileInfo(home()).canonicalFilePath() + "/Documents/Swapped";
        write(grant + "/file.txt", "checked");
        write(m_root.path() + "/outside/swap-secret/file.txt", "SECRET");
        QDir().mkpath(m_root.path() + "/swap/pack");
        sandbox::Profile profile;
        profile.packDir = m_root.path() + "/swap/pack";
        profile.program = "/usr/bin/cat";
        profile.arguments = {grant + "/file.txt"};
        const Result<sandbox::Needs> needs = sandbox::parseNeeds({{"files", QJsonObject{{"read", QJsonArray{"~/Documents/Swapped"}}}}});
        QVERIFY2(needs.ok(), qPrintable(needs.error().message));
        profile.needs = *needs;
        const sandbox::Command command = sandbox::build(profile);
        QVERIFY2(command.refused.isEmpty(), qPrintable(command.refused));
        QVERIFY(boundFd(command, "--ro-bind-fd", grant));
        QVERIFY(QDir().rename(grant, grant + ".real"));
        QVERIFY(QFile::link(m_root.path() + "/outside/swap-secret", grant));
        QProcess process;
        const QList<int> inherited = command.descriptors->numbers();
        process.setProcessEnvironment(sandbox::bwrapEnvironment());
        process.setChildProcessModifier([inherited] { beneath::inheritOnly(inherited); });
        process.start(command.program, command.arguments);
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.readAllStandardOutput(), QByteArray("checked"));
        const sandbox::Command again = sandbox::build(profile);
        QVERIFY2(again.refused.contains("link"), qPrintable(again.refused));
        QVERIFY(!again.arguments.join(' ').contains("swap-secret"));
        QFile::remove(grant);
        QVERIFY(QDir().rename(grant + ".real", grant));
    }

    // Another project's keys are in its root: a grant that holds one or lies in one is refused, a project made with --in
    // anywhere included, and a root registered after the node loaded is caught at the start.
    void aGrantThatHoldsOrLiesInAProjectIsRefused() {
        const QString realHome = QFileInfo(home()).canonicalFilePath();
        QDir(realHome + "/Work").removeRecursively(); // ctest keeps the home between runs
        QDir(realHome + "/Shared").removeRecursively();
        const Result<ProjectEntry> client = createProject("Client", realHome + "/Work/client", {});
        QVERIFY2(client.ok(), qPrintable(client.ok() ? QString() : client.error().message));
        for (const char *bad : {"~/Work", "~/Work/client", "~/Work/client/inputs"})
            QVERIFY2(!sandbox::parseNeeds({{"files", QJsonObject{{"read", QJsonArray{bad}}}}}).ok(), qPrintable(bad));
        // Taken out of the list, its folder (and its keys) stay: a grant in it is still found by its project.json.
        QVERIFY(removeProject("Client").ok());
        QVERIFY(!sandbox::parseNeeds({{"files", QJsonObject{{"read", QJsonArray{"~/Work/client/inputs"}}}}}).ok());

        write(realHome + "/Shared/a.txt", "a");
        const Result<sandbox::Needs> needs = sandbox::parseNeeds({{"files", QJsonObject{{"read", QJsonArray{"~/Shared"}}}}});
        QVERIFY2(needs.ok(), qPrintable(needs.error().message));
        QDir().mkpath(m_root.path() + "/later/pack");
        sandbox::Profile profile;
        profile.packDir = m_root.path() + "/later/pack";
        profile.program = "/usr/bin/true";
        profile.needs = *needs;
        QVERIFY(sandbox::build(profile).refused.isEmpty());
        const Result<ProjectEntry> later = createProject("Later", realHome + "/Shared/later", {});
        QVERIFY2(later.ok(), qPrintable(later.ok() ? QString() : later.error().message));
        const sandbox::Command refused = sandbox::build(profile);
        QVERIFY2(refused.refused.contains("Shared/later"), qPrintable(refused.refused));
        QVERIFY(removeProject("Later").ok());
    }

    // `fugue node test` runs the folder under test in the sandbox even when the person turned it off for the installed pack of
    // the same id and version.
    void aNodeUnderTestIgnoresTheInstalledPacksSandboxOff() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        const NodeType *type = makePack("io.example.undertest");
        TrustStore trust;
        QVERIFY(trust.trust("io.example.undertest", "1.0.0").isEmpty());
        QVERIFY(trust.setUnsandboxed("io.example.undertest", "1.0.0", true).isEmpty());
        CommandKernel kernel(300);
        kernel.setAlwaysSandboxed(true);
        const Result<Values> result = kernel.run(contextFor(*type));
        QVERIFY2(result.ok(), qPrintable(result.error().message));
        QVERIFY2(result->value("text").text().contains("ssh=denied"), qPrintable(result->value("text").text()));
        QVERIFY(trust.forget("io.example.undertest").isEmpty());
    }

    // A setup with uv installs only what a lock file pins: a pack without uv.lock is refused, and `uv sync` runs --frozen.
    void aPackWhoseSetupIsUvShipsAUvLock() {
        const QString source = m_root.path() + "/uv-source/io.example.uvpack";
        write(source + "/pack.json",
              QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.uvpack"}, {"version", "1.0.0"}, {"name", "Uv"},
                                        {"nodes", QJsonArray{"tool"}}, {"setup", QJsonObject{{"command", QJsonArray{"uv", "sync", "--no-dev"}}}}}).toJson());
        write(source + "/tool/node.json",
              QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.uvpack/tool"}, {"version", "1.0.0"}, {"name", "Tool"},
                                        {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}}}).toJson());
        write(source + "/tool/run.sh", "#!/bin/sh\n", true);
        TrustStore trust(m_root.path() + "/trust-uv.json");
        PackPrompts prompts;
        prompts.install = prompts.trust = prompts.runSetup = [](const PackSummary &) { return Answer::Yes; };
        const Result<PackAdded> refused = addPack(source, {}, m_root.path() + "/uv-packs", trust, prompts);
        QVERIFY(!refused.ok());
        QVERIFY2(refused.error().code == "pack_invalid" && refused.error().message.contains("uv.lock"), qPrintable(refused.error().message));
        QVERIFY(!QFileInfo::exists(m_root.path() + "/uv-packs/io.example.uvpack"));
        write(source + "/uv.lock", "version = 1\n");
        const Result<PackSummary> summary = summarizePack(source);
        QVERIFY2(summary.ok(), qPrintable(summary.error().message));
        QCOMPARE(summary->setupCommand, (QStringList{"uv", "sync", "--frozen", "--no-dev"}));
    }

    // --- no sandbox here ---

    void withoutBwrapARunIsRefusedUntilThePersonSaysYesForThisVersion() {
        const NodeType *type = makePack("io.example.fallback");
        sandbox::setBwrapForTest(QString()); // no bwrap
        QVERIFY(!sandbox::available());
        QString error;
        QVERIFY(runHostile(*type, &error).isEmpty());
        QVERIFY2(error.startsWith("sandbox_unavailable"), qPrintable(error));
        QVERIFY2(error.contains("bubblewrap"), qPrintable(error)); // the install hint

        // The yes is the person's, for a trusted version; it is then run as the user.
        TrustStore trust;
        QCOMPARE(trust.setUnsandboxed("io.example.fallback", "1.0.0", true).first().code, QString("pack_untrusted"));
        QVERIFY(trust.trust("io.example.fallback", "1.0.0").isEmpty());
        QVERIFY(!trust.isUnsandboxed("io.example.fallback", "1.0.0"));
        QVERIFY(trust.setUnsandboxed("io.example.fallback", "1.0.0", true).isEmpty());
        QVERIFY(trust.isUnsandboxed("io.example.fallback", "1.0.0"));
        QVERIFY(!trust.isUnsandboxed("io.example.fallback", "1.0.1")); // per version
        const QStringList words = runHostile(*type, &error);
        QVERIFY2(!words.isEmpty(), qPrintable(error));
        QVERIFY2(words.contains("ssh=ALLOWED"), qPrintable(words.join(' '))); // as the user: nothing is confined

        // Taking the trust back takes the yes with it.
        QVERIFY(trust.forget("io.example.fallback").isEmpty());
        QVERIFY(!trust.isUnsandboxed("io.example.fallback", "1.0.0"));
        QVERIFY(runHostile(*type, &error).isEmpty());
    }

    void aCommunityPackIsNeverRunWithoutTheSandbox() {
        const NodeType *type = makePack("io.example.community", {}, true);
        QVERIFY(type->packCommunity);
        sandbox::setBwrapForTest(QString());
        TrustStore trust;
        QVERIFY(trust.trust("io.example.community", "1.0.0").isEmpty());
        QVERIFY(trust.setUnsandboxed("io.example.community", "1.0.0", true).isEmpty()); // the file may hold it; the runner ignores it
        QString error;
        QVERIFY(runHostile(*type, &error).isEmpty());
        QVERIFY2(error.startsWith("sandbox_unavailable"), qPrintable(error));
    }

    void aSetupIsRefusedWithoutASandboxToo() {
        const QString source = m_root.path() + "/nosetup-source/io.example.nosandbox";
        write(source + "/pack.json",
              QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.nosandbox"}, {"version", "1.0.0"}, {"name", "Setup"},
                                        {"nodes", QJsonArray{"tool"}},
                                        {"setup", QJsonObject{{"command", QJsonArray{"touch", "ran"}}}}}).toJson());
        write(source + "/tool/node.json",
              QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.nosandbox/tool"}, {"version", "1.0.0"}, {"name", "Tool"},
                                        {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}}}).toJson());
        write(source + "/tool/run.sh", "#!/bin/sh\n", true);
        sandbox::setBwrapForTest(QString());
        TrustStore trust(m_root.path() + "/trust-nosandbox.json");
        PackPrompts prompts;
        prompts.install = prompts.trust = prompts.runSetup = [](const PackSummary &) { return Answer::Yes; };
        const QString packs = m_root.path() + "/nosandbox-packs";
        const Result<PackAdded> added = addPack(source, {}, packs, trust, prompts);
        QVERIFY(!added.ok());
        QCOMPARE(added.error().code, QString("sandbox_unavailable"));
        QVERIFY(!QFileInfo::exists(packs + "/io.example.nosandbox")); // nothing was installed
    }

    void thePersonsYesToTurnTheSandboxOffIsAskedAndStored() {
        const QString source = m_root.path() + "/off-source/io.example.off";
        write(source + "/pack.json",
              QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.off"}, {"version", "1.0.0"}, {"name", "Off"}, {"nodes", QJsonArray{"tool"}}}).toJson());
        write(source + "/tool/node.json",
              QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.off/tool"}, {"version", "1.0.0"}, {"name", "Tool"},
                                        {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}}}).toJson());
        write(source + "/tool/run.sh", "#!/bin/sh\n", true);
        TrustStore trust(m_root.path() + "/trust-off.json");
        const QString packs = m_root.path() + "/off-packs";
        PackPrompts yes;
        yes.install = yes.trust = yes.runSetup = [](const PackSummary &) { return Answer::Yes; };
        QVERIFY(addPack(source, {}, packs, trust, yes).ok());
        QVERIFY(trustPack("io.example.off", packs, trust, yes).ok());
        PackPrompts ask;
        int asked = 0;
        ask.unsandbox = [&asked](const PackSummary &) { ++asked; return Answer::No; };
        QCOMPARE(setPackSandbox("io.example.off", false, packs, trust, ask).error().code, QString("cancelled"));
        QCOMPARE(asked, 1);
        QVERIFY(!trust.isUnsandboxed("io.example.off", "1.0.0"));
        QCOMPARE(setPackSandbox("io.example.off", false, packs, trust, PackPrompts{}).error().code, QString("needs_confirmation")); // no person
        ask.unsandbox = [](const PackSummary &) { return Answer::Yes; };
        QVERIFY(setPackSandbox("io.example.off", false, packs, trust, ask).ok());
        QVERIFY(trust.isUnsandboxed("io.example.off", "1.0.0"));
        QVERIFY(setPackSandbox("io.example.off", true, packs, trust, PackPrompts{}).ok()); // on again: no question
        QVERIFY(!trust.isUnsandboxed("io.example.off", "1.0.0"));
    }
};

QTEST_GUILESS_MAIN(TestSandbox)
#include "test_sandbox.moc"
