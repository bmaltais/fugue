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
    const NodeType *makePack(const QString &id, const QJsonObject &needs = {}, bool community = false, const QByteArray &script = {}) {
        const QString dir = m_root.path() + "/packs/" + id;
        QJsonObject pack{{"manifest", 1}, {"id", id}, {"version", "1.0.0"}, {"name", "Hostile"}, {"nodes", QJsonArray{"hostile"}}};
        if (community)
            pack["community"] = true;
        write(dir + "/pack.json", QJsonDocument(pack).toJson());
        QJsonObject node{{"manifest", 1}, {"id", id + "/hostile"}, {"version", "1.0.0"}, {"name", "Hostile"},
                         {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                         {"inputs", QJsonArray{}},
                         {"outputs", QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}, {"optional", true}}}}};
        if (!needs.isEmpty())
            node["needs"] = needs;
        write(dir + "/hostile/node.json", QJsonDocument(node).toJson());
        QByteArray body = script.isEmpty() ? hostileScript() : script;
        body.replace("@SSH@", (home() + "/.ssh/id_test").toUtf8());
        body.replace("@SECRETS@", secretsFile().toUtf8());
        body.replace("@CONFIG@", configDir().toUtf8());
        body.replace("@OUTSIDE@", (m_root.path() + "/outside").toUtf8());
        body.replace("@PORT@", QByteArray::number(m_server.serverPort()));
        write(dir + "/hostile/run.sh", body, true);
        const LoadedPack loaded = loadPack(m_registry, dir, PackOrigin::Disk, PackDigest::Leave);
        if (!loaded.enabled)
            qFatal("pack did not load: %s", qPrintable(loaded.errors.isEmpty() ? QString() : loaded.errors.first().message));
        return m_registry.nodeType(id + "/hostile");
    }

    NodeContext contextFor(const NodeType &type) {
        NodeContext context;
        context.node = "n1";
        context.run = "r1";
        context.type = &type;
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
        QVERIFY(hasPair(args, "--bind", profile.packDir, profile.packDir));
        QVERIFY(hasPair(args, "--bind-try", profile.uvCache, profile.uvCache));
        QVERIFY(!args.contains("--chdir") || hasPair(args, "--chdir", profile.packDir));
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

    void theDenylistRefusesWhatHoldsKeysAndFugueItself() {
        for (const QString &bad : {QStringLiteral("~/.ssh"), QStringLiteral("~/.ssh/id_rsa"), QStringLiteral("~/.gnupg"), QStringLiteral("~/.config/fugue"),
                                   QStringLiteral("~/.local/share/uv/credentials"), QStringLiteral("~/.local/share/keyrings"), QStringLiteral("~"),
                                   QStringLiteral("/"), QStringLiteral("/etc"), configDir(), secretsFile(), stateDir() + "/trust.json",
                                   QStringLiteral("~/Documents/../.ssh"), QStringLiteral("relative/path")}) {
            const Result<sandbox::Needs> parsed = sandbox::parseNeeds({{"files", QJsonObject{{"read", QJsonArray{bad}}}}});
            QVERIFY2(!parsed.ok(), qPrintable(bad));
            const Result<sandbox::Needs> writing = sandbox::parseNeeds({{"files", QJsonObject{{"write", QJsonArray{bad}}}}});
            QVERIFY2(!writing.ok(), qPrintable(bad));
        }
        const Result<sandbox::Needs> good = sandbox::parseNeeds({{"network", true}, {"gpu", QJsonObject{{"api", "any"}}},
                                                                 {"files", QJsonObject{{"read", QJsonArray{"~/Documents/Scans"}}, {"write", QJsonArray{"/srv/out"}}}}});
        QVERIFY2(good.ok(), qPrintable(good.error().message));
        QVERIFY(good->network && good->gpu);
        QCOMPARE(good->readPaths, QStringList{home() + "/Documents/Scans"});
        QCOMPARE(sandbox::plainLines(*good, true),
                 (QStringList{"Can use the network", "Can use the GPU", "Can read ~/Documents/Scans", "Can write /srv/out"}));
        QVERIFY(sandbox::plainLines({}, false).first().startsWith("NOT sandboxed"));
        QVERIFY(sandbox::plainLines({}, true).first().startsWith("Runs in a sandbox"));
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
        QDir().mkpath(home() + "/Documents/Out");
        const QByteArray script = "#!/bin/bash\ncat > /dev/null\nr=\"\"\n"
                                  "check() { name=$1; shift; if \"$@\" > /dev/null 2>&1; then r=\"$r $name=ALLOWED\"; else r=\"$r $name=denied\"; fi; }\n"
                                  "check scan cat \"$HOME_REAL/Documents/Scans/page.txt\"\n"
                                  "check scanwrite bash -c \"echo x > '$HOME_REAL/Documents/Scans/new.txt'\"\n"
                                  "check other cat \"$HOME_REAL/Documents/Private/other.txt\"\n"
                                  "check out bash -c \"echo x > '$HOME_REAL/Documents/Out/new.txt'\"\n"
                                  "printf '{\"outputs\":{\"text\":\"%s\"}}\\n' \"$r\"\n";
        QByteArray body = script;
        body.replace("$HOME_REAL", home().toUtf8());
        const NodeType *type = makePack("io.example.files",
                                        {{"files", QJsonObject{{"read", QJsonArray{"~/Documents/Scans"}}, {"write", QJsonArray{"~/Documents/Out"}}}}}, false, body);
        const QStringList words = runHostile(*type);
        QCOMPARE(words, (QStringList{"scan=ALLOWED", "scanwrite=denied", "other=denied", "out=ALLOWED"}));
    }

    void setupRunsInTheSandboxWithTheNetworkAndWritesOnlyItsOwnFolder() {
        if (!sandbox::available())
            QSKIP("bubblewrap or user namespaces are not available here");
        const QString source = m_root.path() + "/setup-source/io.example.setup";
        const QString command = QStringLiteral("echo made > .venv-made; echo x > '%1/outside/from-setup' 2>/dev/null; "
                                               "cat '%2' > seen-ssh 2>/dev/null; exec 3<>/dev/tcp/127.0.0.1/%3 && echo net > net-ok; true")
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
        QCOMPARE(readAll(packs + "/io.example.setup/.venv-made"), QByteArray("made\n")); // its folder is writable
        QVERIFY(QFileInfo::exists(packs + "/io.example.setup/net-ok"));                     // the network is on
        QVERIFY(!QFileInfo::exists(m_root.path() + "/outside/from-setup"));                // nothing else is
        QVERIFY(readAll(packs + "/io.example.setup/seen-ssh").isEmpty());                   // no home
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
