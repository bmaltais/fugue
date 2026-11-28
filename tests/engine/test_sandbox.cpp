// The sandbox of a pack's programs (spec 6.7.1, #731): the bwrap command line (pure), the denylist of `needs.files`, and
// real programs run through CommandKernel and a pack's setup, which a hostile pack tries to get out of. The runs need bwrap
// and user namespaces; without them the run tests skip (the fallback test needs neither).
//
// Seam: sandbox::build / parseNeeds, CommandKernel::run() on a node loaded from a pack folder, addPack() for the setup.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>

#include "engine/command_node.h"
#include "engine/file_hashes.h"
#include "engine/pack_loader.h"
#include "engine/pack_manager.h"
#include "engine/pack_secrets.h"
#include "engine/paths.h"
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
        for (const char *flag : {"--unshare-all", "--unshare-user", "--disable-userns", "--new-session", "--die-with-parent", "--clearenv"})
            QVERIFY2(args.contains(flag), flag);
        QVERIFY(!args.contains("--share-net")); // no `network`: the network namespace stays empty
        QVERIFY(hasPair(args, "--ro-bind", profile.packDir, profile.packDir));
        QVERIFY(hasPair(args, "--bind", profile.packDir + "/.venv", profile.packDir + "/.venv")); // its environment folder
        QVERIFY(hasPair(args, "--bind", profile.workDir, profile.workDir));
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
        const QStringList withLock = sandbox::build(profile).arguments;
        QVERIFY(hasPair(withLock, "--ro-bind", profile.packDir, profile.packDir)); // the pack's own files are read-only
        QVERIFY(hasPair(withLock, "--bind", profile.packDir + "/.venv", profile.packDir + "/.venv"));
        QVERIFY(hasPair(withLock, "--bind", profile.packDir + "/uv.lock", profile.packDir + "/uv.lock"));
        QVERIFY(!hasPair(withLock, "--bind", profile.packDir, profile.packDir));
        QVERIFY(hasPair(withLock, "--bind-try", profile.uvCache, profile.uvCache));
        QVERIFY2(profile.uvCache.startsWith(cacheDir()), qPrintable(profile.uvCache)); // Fugue's own cache, one per pack
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
        profile.allowedRoots = {models};
        profile.handedFiles = {models + "/real.onnx", models + "/inside.onnx", models + "/outside.onnx", models + "/key.onnx",
                               home() + "/.ssh/handed", models + "/missing.onnx"};
        const sandbox::Command command = sandbox::build(profile);
        QVERIFY(hasPair(command.arguments, "--ro-bind", models + "/real.onnx", models + "/real.onnx"));
        QVERIFY(hasPair(command.arguments, "--symlink", models + "/real.onnx", models + "/inside.onnx")); // the link points at the bound target
        QVERIFY(!command.arguments.join(' ').contains(elsewhere)); // a link out of the roots: left out
        QVERIFY(!command.arguments.join(' ').contains(".ssh")); // and a denied path never, linked or handed directly
        QCOMPARE(command.skipped.size(), 4);
    }

    // --- needs.files and the denylist ---

    void grantsAreAnAllowlist() {
        const QStringList readBad{"~/.ssh", "~/.ssh/id_rsa", "~/.gnupg", "~/.config/fugue", "~/.local/share/uv/credentials", "~/.local/share/keyrings",
                                  "~", "/", "/etc", "~/Documents/../.ssh", "relative/path", "~/.cache/huggingface/token", "~/.git-credentials",
                                  "~/.claude", "~/.mozilla/firefox", "~/.config/chromium", "~/.bashrc", "~/.local/bin", "~/.config/systemd/user",
                                  "~/.config/hypr", "~/.local/share/applications", "~/.anything-with-a-dot/inside", "/tmp/tmux-1000", "/var/tmp",
                                  "/run/user/1000", "/dev", "/proc", "/sys", "/boot", "/root", configDir(), secretsFile(), stateDir() + "/trust.json",
                                  dataDir() + "/packs", cacheDir(), baseConfigDir(), baseDataDir() + "/projects"};
        for (const QString &bad : readBad)
            QVERIFY2(!sandbox::parseNeeds({{"files", QJsonObject{{"read", QJsonArray{bad}}}}}).ok(), qPrintable("read " + bad));
        // Writing is allowed in the output folder only.
        const QStringList writeBad{"~/Documents/Out", "~/.local/bin", "~/.bashrc", "~/.config/autostart", "~/.config/systemd/user", "~/.config/hypr/hyprland.conf",
                                   "~/.local/share/applications", "~", "/tmp", "/srv/out", outputDir() + "/../elsewhere", configDir()};
        for (const QString &bad : writeBad)
            QVERIFY2(!sandbox::parseNeeds({{"files", QJsonObject{{"write", QJsonArray{bad}}}}}).ok(), qPrintable("write " + bad));
        const Result<sandbox::Needs> good = sandbox::parseNeeds({{"network", true}, {"gpu", QJsonObject{{"api", "any"}}},
                                                                 {"files", QJsonObject{{"read", QJsonArray{"~/Documents/Scans"}}, {"write", QJsonArray{outputDir() + "/Out"}}}}});
        QVERIFY2(good.ok(), qPrintable(good.error().message));
        QVERIFY(good->network && good->gpu);
        QCOMPARE(good->readPaths, QStringList{home() + "/Documents/Scans"});
        QCOMPARE(sandbox::plainLines(*good, true),
                 (QStringList{"Can use the network", "Can reach services on this computer", "Can use the GPU", "Can read ~/Documents/Scans",
                              "Can write ~/Pictures/Fugue/Out"}));
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
        profile.gpuNodes.clear();
        profile.needs.gpuApi = "vulkan";
        QVERIFY(hasPair(sandbox::build(profile).arguments, "--dev-bind-try", "/dev/dri", "/dev/dri"));
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

    void aNodeWithADeniedPathDoesNotLoad() {
        const QJsonObject manifest{{"manifest", 1}, {"id", "io.example.tools/bad"}, {"version", "1.0.0"}, {"name", "Bad"},
                                   {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                                   {"needs", QJsonObject{{"files", QJsonObject{{"read", QJsonArray{"~/.ssh"}}}}}}};
        const Result<NodeType> type = NodeType::fromJson(manifest);
        QVERIFY(!type.ok());
        QVERIFY2(type.error().message.contains(".ssh"), qPrintable(type.error().message));
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
        QDir().mkpath(outputDir() + "/Out");
        const QByteArray script = "#!/bin/bash\ncat > /dev/null\nr=\"\"\n"
                                  "check() { name=$1; shift; if \"$@\" > /dev/null 2>&1; then r=\"$r $name=ALLOWED\"; else r=\"$r $name=denied\"; fi; }\n"
                                  "check scan cat \"$HOME_REAL/Documents/Scans/page.txt\"\n"
                                  "check scanwrite bash -c \"echo x > '$HOME_REAL/Documents/Scans/new.txt'\"\n"
                                  "check other cat \"$HOME_REAL/Documents/Private/other.txt\"\n"
                                  "check out bash -c \"echo x > '$OUT_REAL/Out/new.txt'\"\n"
                                  "printf '{\"outputs\":{\"text\":\"%s\"}}\\n' \"$r\"\n";
        QByteArray body = script;
        body.replace("$HOME_REAL", home().toUtf8());
        body.replace("$OUT_REAL", outputDir().toUtf8());
        const NodeType *type = makePack("io.example.files",
                                        {{"files", QJsonObject{{"read", QJsonArray{"~/Documents/Scans"}}, {"write", QJsonArray{outputDir() + "/Out"}}}}}, false, body);
        const QStringList words = runHostile(*type);
        QCOMPARE(words, (QStringList{"scan=ALLOWED", "scanwrite=denied", "other=denied", "out=ALLOWED"}));
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

    void aSandboxedProgramThatSeesAModelOrMediaFileOnlyThroughItsHandle() {
        // A handle's "path" is what is bound; another "path" text inside a value is nothing to the sandbox.
        QDir().mkpath(m_root.path() + "/arg4/pack");
        sandbox::Profile profile;
        profile.packDir = m_root.path() + "/arg4/pack";
        profile.program = "/usr/bin/true";
        profile.handedFiles = {};
        QVERIFY(!sandbox::build(profile).arguments.join(' ').contains(".ssh"));
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
