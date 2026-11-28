// `fugue pack` (spec 6.6, 13.6) as the real binary: everything but `list` needs a person.
// With no terminal each command fails with `needs_confirmation` and changes nothing; the
// terminal path is driven through `script`, which gives the program a pseudo-terminal.

#include "../test_paths.h"
#include "engine/sandbox.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QString firstCode() const { return json().value("errors").toArray().at(0).toObject().value("code").toString(); }
};

QString shellQuote(const QString &text) { return "'" + QString(text).replace("'", "'\\''") + "'"; }

} // namespace

class TestCliPacks : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QString packs() const { return m_dir.filePath("data/fugue/packs"); }

    void write(const QString &path, const QByteArray &bytes) const {
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
        file.write(bytes);
    }

    QString makePack(const QString &root, const QString &id, const QString &version, const QJsonObject &needs = {},
                     bool community = false) const {
        const QString dir = root + "/" + id;
        QJsonObject pack{{"manifest", 1}, {"id", id}, {"version", version}, {"name", "Test"}, {"nodes", QJsonArray{"tool"}}};
        if (community)
            pack["community"] = true;
        write(dir + "/pack.json", QJsonDocument(pack).toJson());
        QJsonObject node{{"manifest", 1}, {"id", id + "/tool"}, {"version", "1.0.0"}, {"name", "Tool"},
                         {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                         {"inputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}},
                         {"outputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}}};
        if (!needs.isEmpty())
            node["needs"] = needs;
        write(dir + "/tool/node.json", QJsonDocument(node).toJson());
        return dir;
    }

    QProcessEnvironment environment() const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.remove("DISPLAY");
        env.remove("WAYLAND_DISPLAY");
        env.insert("XDG_DATA_HOME", m_dir.filePath("data"));
        env.insert("XDG_STATE_HOME", m_dir.filePath("state"));
        env.insert("XDG_CACHE_HOME", m_dir.filePath("cache"));
        env.insert("XDG_CONFIG_HOME", m_dir.filePath("config"));
        env.insert("HOME", m_dir.filePath("home"));
        return env;
    }

    // No terminal: stdin is closed and the output is captured.
    Outcome fugue(const QStringList &args) const {
        QProcess process;
        process.setProcessEnvironment(environment());
        process.start(fugueBinary(), args);
        process.closeWriteChannel();
        Outcome outcome;
        if (!process.waitForFinished(120000)) {
            process.kill();
            process.waitForFinished();
            return outcome;
        }
        outcome.exitCode = process.exitCode();
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    // On a pseudo-terminal, typing `typed`. Returns what the program showed.
    QString atTerminal(const QStringList &args, const QByteArray &typed, int *exitCode) const {
        QString command = shellQuote(fugueBinary());
        for (const QString &arg : args)
            command += " " + shellQuote(arg);
        QProcess process;
        process.setProcessEnvironment(environment());
        process.start("script", {"-qec", command, "/dev/null"});
        process.write(typed);
        process.closeWriteChannel();
        if (!process.waitForFinished(120000)) {
            process.kill();
            process.waitForFinished();
            *exitCode = -1;
            return {};
        }
        *exitCode = process.exitCode();
        return QString::fromUtf8(process.readAllStandardOutput());
    }

private slots:
    void listWorksWithoutAPerson() {
        const Outcome none = fugue({"pack", "list", "--json"});
        QCOMPARE(none.exitCode, 0);
        QVERIFY(none.json().value("data").toObject().value("packs").toArray().isEmpty());

        makePack(packs(), "io.example.tools", "1.0.0"); // copied in by hand
        const Outcome some = fugue({"packs", "list", "--json"}); // the plural is an alias
        const QJsonObject pack = some.json().value("data").toObject().value("packs").toArray().at(0).toObject();
        QCOMPARE(pack.value("id").toString(), QString("io.example.tools"));
        QCOMPARE(pack.value("enabled").toBool(), true);
        QCOMPARE(pack.value("trusted").toBool(), false);
        QVERIFY(QString::fromUtf8(fugue({"pack", "list"}).out).contains("untrusted"));
        QDir(packs()).removeRecursively();
    }

    void everythingElseNeedsAPerson() {
        const QString source = makePack(m_dir.filePath("source"), "io.example.tools", "1.0.0");
        const Outcome add = fugue({"pack", "add", source, "--json"});
        QCOMPARE(add.exitCode, 1);
        QCOMPARE(add.firstCode(), QString("needs_confirmation"));
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists());
        QVERIFY(QDir(packs()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty()); // no staging left

        makePack(packs(), "io.example.tools", "1.0.0");
        for (const QStringList &args : {QStringList{"pack", "trust", "io.example.tools", "--json"},
                                        QStringList{"pack", "remove", "io.example.tools", "--json"},
                                        QStringList{"pack", "update", "--json"}}) {
            const Outcome outcome = fugue(args);
            if (args.at(1) == "update") {
                QCOMPARE(outcome.exitCode, 0); // nothing has a source, so nothing to ask
                continue;
            }
            QCOMPARE(outcome.exitCode, 1);
            QCOMPARE(outcome.firstCode(), QString("needs_confirmation"));
        }
        QVERIFY(QFileInfo(packs() + "/io.example.tools").isDir());
        QVERIFY(!QFile::exists(m_dir.filePath("state/fugue/trust.json")));
        QDir(packs()).removeRecursively();
    }

    // The sandbox in plain words (6.7.1, #731): list and info say whether a pack is sandboxed and what it asked for; turning the
    // sandbox off is a person's yes and is never offered for a community pack.
    void infoAndListSayWhatThePackMayDoAndTheSandboxCanBeTurnedOffByAPerson() {
        if (QStandardPaths::findExecutable("script").isEmpty())
            QSKIP("script (util-linux) is needed to give the program a terminal");
        makePack(packs(), "io.example.net", "1.0.0", {{"network", true}, {"files", QJsonObject{{"read", QJsonArray{"~/Documents/Scans"}}}}});
        makePack(packs(), "io.example.shared", "2.0.0", {}, true);
        write(m_dir.filePath("state/fugue/trust.json"),
              QJsonDocument(QJsonObject{{"packs", QJsonObject{{"io.example.net", QJsonArray{"1.0.0"}}, {"io.example.shared", QJsonArray{"2.0.0"}}}}}).toJson());

        const QJsonObject info = fugue({"pack", "info", "io.example.net", "--json"}).json().value("data").toObject();
        QCOMPARE(info.value("sandboxed").toBool(), true);
        QCOMPARE(info.value("needs").toObject().value("network").toBool(), true);
        QCOMPARE(info.value("needs_lines").toArray().at(0).toString(), QString("Can use the network"));
        QCOMPARE(info.value("needs_lines").toArray().at(1).toString(), QString("Can read ~/Documents/Scans"));
        QVERIFY(QString::fromUtf8(fugue({"pack", "info", "io.example.net"}).out).contains("Can use the network"));

        // Without a person the sandbox stays on; a community pack's can never be turned off.
        QCOMPARE(fugue({"pack", "sandbox-off", "io.example.net", "--json"}).firstCode(), QString("needs_confirmation"));
        int exitCode = -1;
        const QString refused = atTerminal({"pack", "sandbox-off", "io.example.shared"}, "y\n", &exitCode);
        QCOMPARE(exitCode, 1);
        QVERIFY2(refused.contains("community"), qPrintable(refused));

        const QString asked = atTerminal({"pack", "sandbox-off", "io.example.net"}, "y\n", &exitCode);
        QCOMPARE(exitCode, 0);
        QVERIFY2(asked.contains("NOT sandboxed"), qPrintable(asked));
        const QJsonArray listed = fugue({"pack", "list", "--json"}).json().value("data").toObject().value("packs").toArray();
        for (const QJsonValue &entry : listed) {
            const QJsonObject pack = entry.toObject();
            QCOMPARE(pack.value("sandboxed").toBool(), pack.value("id").toString() != "io.example.net");
            if (pack.value("id").toString() == "io.example.net")
                QCOMPARE(pack.value("needs_lines").toArray().at(0).toString(), QString("NOT sandboxed: this pack's program runs as you."));
        }
        QVERIFY(QString::fromUtf8(fugue({"pack", "list"}).out).contains("NOT sandboxed"));
        QCOMPARE(fugue({"pack", "sandbox-on", "io.example.net", "--json"}).exitCode, 0);
        QCOMPARE(fugue({"pack", "info", "io.example.net", "--json"}).json().value("data").toObject().value("sandboxed").toBool(), true);
        QDir(packs()).removeRecursively();
        QFile::remove(m_dir.filePath("state/fugue/trust.json"));
    }

    void aGitPackWithoutAPinIsAUsageError() {
        const Outcome outcome = fugue({"pack", "add", "https://example.com/x.git", "--json"});
        QCOMPARE(outcome.firstCode(), QString("usage"));
    }

    void aPersonAtATerminalInstallsTrustsAndRemoves() {
        if (QStandardPaths::findExecutable("script").isEmpty())
            QSKIP("script (util-linux) is needed to give the program a terminal");
        const QString source = makePack(m_dir.filePath("source"), "io.example.tools", "1.0.0");
        int exitCode = -1;

        QString shown = atTerminal({"pack", "add", source}, "n\n", &exitCode);
        QCOMPARE(exitCode, 1);
        QVERIFY(shown.contains("io.example.tools"));
        QVERIFY(shown.contains("Install this pack and trust it?"));
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists());

        shown = atTerminal({"pack", "add", source}, "y\n", &exitCode);
        QCOMPARE(exitCode, 0);
        QVERIFY(QFileInfo(packs() + "/io.example.tools/pack.json").isFile());
        QVERIFY2(shown.contains("Runs in a sandbox"), qPrintable(shown)); // the plain line of a pack that asks for nothing

        // The one yes installed and trusted it: no second question.
        QCOMPARE(fugue({"pack", "list", "--json"}).json().value("data").toObject().value("packs").toArray().at(0).toObject().value("trusted").toBool(), true);

        atTerminal({"pack", "remove", "io.example.tools"}, "y\n", &exitCode);
        QCOMPARE(exitCode, 0);
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists());
        QVERIFY(fugue({"pack", "list", "--json"}).json().value("data").toObject().value("packs").toArray().isEmpty());
    }

    void setupShowsTheExactCommand() {
        if (!fugue::sandbox::available())
            QSKIP("the setup runs in the sandbox, and bubblewrap or user namespaces are not available here");
        if (QStandardPaths::findExecutable("script").isEmpty())
            QSKIP("script (util-linux) is needed to give the program a terminal");
        const QString source = makePack(m_dir.filePath("source2"), "io.example.setup", "1.0.0");
        QFile file(source + "/pack.json");
        QVERIFY(file.open(QIODevice::ReadWrite));
        QJsonObject pack = QJsonDocument::fromJson(file.readAll()).object();
        pack["setup"] = QJsonObject{{"command", QJsonArray{"touch", "made it"}}, {"description", "Make a marker"}};
        file.resize(0);
        file.seek(0);
        file.write(QJsonDocument(pack).toJson());
        file.close();

        int exitCode = -1;
        const QString shown = atTerminal({"pack", "add", source}, "y\n", &exitCode); // one yes: install, setup and trust
        QVERIFY2(exitCode == 0, qPrintable(shown));
        QVERIFY2(shown.contains("touch 'made it'"), qPrintable(shown)); // the command is in the list it asked about
        QVERIFY(QFileInfo(packs() + "/io.example.setup/made it").exists());
    }

    // Review of #379: what cannot be made ready is not "ready": the problems are reported with a non-zero exit.
    void prepareWithProblemsIsNotReady() {
        const QString workflow = m_dir.filePath("nosource.fugue");
        write(workflow, QJsonDocument(QJsonObject{
            {"fugue", 1},
            {"requires", QJsonObject{{"packs", QJsonObject{{"io.example.tools", QJsonObject{{"version", "1.0.0"}}}}}}},
            {"graph", QJsonObject{{"nodes", QJsonObject{{"t", QJsonObject{{"type", "io.example.tools/tool"}, {"version", "1.0.0"},
                                                                             {"inputs", QJsonObject{}}}}}}}}}).toJson());
        const Outcome outcome = fugue({"workflow", "prepare", workflow, "--json"});
        QCOMPARE(outcome.exitCode, 1);
        QVERIFY(!outcome.json().value("ok").toBool());
        QVERIFY(outcome.out.contains("pack_source_unknown"));
    }

    // #379: `workflow prepare` lists what a workflow needs and asks once; with no terminal it is needs_confirmation.
    void prepareListsAndAsksOnce() {
        const QString source = makePack(m_dir.filePath("source3"), "io.example.tools", "1.0.0");
        const QString workflow = m_dir.filePath("needs.fugue");
        write(workflow, QJsonDocument(QJsonObject{
            {"fugue", 1},
            {"requires", QJsonObject{{"packs", QJsonObject{{"io.example.tools", QJsonObject{{"version", "1.0.0"}, {"source", source}}}}}}},
            {"graph", QJsonObject{{"nodes", QJsonObject{{"t", QJsonObject{{"type", "io.example.tools/tool"}, {"version", "1.0.0"},
                                                                             {"inputs", QJsonObject{}}}}}}}}}).toJson());
        const Outcome none = fugue({"workflow", "prepare", workflow, "--json"});
        QCOMPARE(none.exitCode, 1);
        QCOMPARE(none.firstCode(), QString("needs_confirmation"));
        QVERIFY(none.out.contains("install 1 pack"));
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists());
        if (QStandardPaths::findExecutable("script").isEmpty())
            return;
        int exitCode = -1;
        QString shown = atTerminal({"workflow", "prepare", workflow}, "n\n", &exitCode);
        QCOMPARE(exitCode, 1);
        QVERIFY(shown.contains("Install and trust all?"));
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists());
        shown = atTerminal({"workflow", "prepare", workflow}, "y\n", &exitCode);
        QVERIFY2(exitCode == 0, qPrintable(shown));
        QVERIFY(QFileInfo(packs() + "/io.example.tools/pack.json").isFile());
        QVERIFY(QFileInfo(m_dir.filePath("state/fugue/trust.json")).exists());
        QCOMPARE(fugue({"workflow", "prepare", workflow, "--json"}).exitCode, 0); // nothing left: no question
    }
};

QTEST_GUILESS_MAIN(TestCliPacks)
#include "test_cli_packs.moc"
