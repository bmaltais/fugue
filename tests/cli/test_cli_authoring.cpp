// `fugue node new` and `fugue node test` (spec 6.11) as the real binary, and the example pack
// of the user docs (examples/packs), which must pass its own tests.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QString firstCode() const { return json().value("errors").toArray().at(0).toObject().value("code").toString(); }
    QString firstMessage() const { return json().value("errors").toArray().at(0).toObject().value("message").toString(); }
};

} // namespace

class TestCliAuthoring : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

    Outcome fugue(const QStringList &args, const QString &cwd) const {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.insert("XDG_DATA_HOME", m_dir.filePath("data"));
        env.insert("XDG_STATE_HOME", m_dir.filePath("state"));
        env.insert("XDG_CACHE_HOME", m_dir.filePath("cache"));
        env.insert("XDG_CONFIG_HOME", m_dir.filePath("config"));
        env.insert("HOME", m_dir.filePath("home"));
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(cwd);
        process.start(FUGUE_BINARY, QStringList(args) << "--json");
        process.closeWriteChannel();
        Outcome outcome;
        if (!process.waitForFinished(60000)) {
            process.kill();
            process.waitForFinished();
            return outcome;
        }
        outcome.exitCode = process.exitCode();
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    QString work(const QString &name) const {
        QDir().mkpath(m_dir.filePath(name));
        return m_dir.filePath(name);
    }

    static void rewrite(const QString &path, const QByteArray &from, const QByteArray &to) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QByteArray bytes = file.readAll();
        QVERIFY(bytes.contains(from));
        bytes.replace(from, to);
        file.resize(0);
        file.write(bytes);
    }

private slots:
    void newMakesAPackThatPassesItsOwnTest() {
        const QString cwd = work("new");
        const Outcome made = fugue({"node", "new", "greeter"}, cwd);
        QCOMPARE(made.exitCode, 0);
        QVERIFY(QFileInfo(cwd + "/io.example.greeter/pack.json").isFile());
        QVERIFY(QFileInfo(cwd + "/io.example.greeter/greeter/run.sh").isExecutable());
        const Outcome tested = fugue({"node", "test", "io.example.greeter/greeter"}, cwd);
        QVERIFY2(tested.exitCode == 0, tested.out.constData());
        QCOMPARE(tested.json().value("data").toObject().value("tested").toInt(), 2);
        // The same, given the pack folder.
        QCOMPARE(fugue({"node", "test", "io.example.greeter"}, cwd).exitCode, 0);
    }

    void newInsideAPackAddsANode() {
        const QString cwd = work("add");
        QCOMPARE(fugue({"node", "new", "first", "--id", "io.example.two"}, cwd).exitCode, 0);
        const QString pack = cwd + "/io.example.two";
        QCOMPARE(fugue({"node", "new", "second"}, pack).exitCode, 0);
        QFile file(pack + "/pack.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonArray nodes = QJsonDocument::fromJson(file.readAll()).object().value("nodes").toArray();
        QCOMPARE(nodes, (QJsonArray{"first", "second"}));
        QCOMPARE(fugue({"node", "test", pack}, cwd).json().value("data").toObject().value("tested").toInt(), 4);
        const Outcome again = fugue({"node", "new", "second"}, pack);
        QCOMPARE(again.exitCode, 1);
        QCOMPARE(again.firstCode(), QString("pack_exists"));
    }

    void newRefusesBadNamesAndIds() {
        const QString cwd = work("bad");
        QCOMPARE(fugue({"node", "new", "Bad-Name"}, cwd).firstCode(), QString("usage"));
        QCOMPARE(fugue({"node", "new", "ok", "--id", "NotReverseDns"}, cwd).firstCode(), QString("usage"));
        QVERIFY(QDir(cwd).isEmpty());
    }

    void aWrongExpectationFailsTheTest() {
        const QString cwd = work("fail");
        QCOMPARE(fugue({"node", "new", "greeter"}, cwd).exitCode, 0);
        rewrite(cwd + "/io.example.greeter/greeter/test.json", "Hello, Fugue!", "Hullo, Fugue!");
        const Outcome tested = fugue({"node", "test", "io.example.greeter/greeter"}, cwd);
        QCOMPARE(tested.exitCode, 1);
        QCOMPARE(tested.firstCode(), QString("node_test_failed"));
        QVERIFY2(tested.firstMessage().contains("greets"), qPrintable(tested.firstMessage()));
    }

    void aProgramThatFailsReportsItsOwnError() {
        const QString cwd = work("crash");
        QCOMPARE(fugue({"node", "new", "greeter"}, cwd).exitCode, 0);
        QFile script(cwd + "/io.example.greeter/greeter/run.sh");
        QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
        script.write("#!/bin/bash\ncat >/dev/null\necho '{\"op\": \"error\", \"code\": \"bad_input\", \"message\": \"too small\"}'\n");
        script.close();
        const Outcome tested = fugue({"node", "test", "io.example.greeter/greeter"}, cwd);
        QCOMPARE(tested.exitCode, 1);
        QVERIFY2(tested.firstMessage().contains("too small"), qPrintable(tested.firstMessage()));
    }

    void aMissingRequiredOutputIsABadOutput() {
        const QString cwd = work("noout");
        QCOMPARE(fugue({"node", "new", "greeter"}, cwd).exitCode, 0);
        QFile script(cwd + "/io.example.greeter/greeter/run.sh");
        QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
        script.write("#!/bin/bash\ncat >/dev/null\necho '{\"outputs\": {}}'\n");
        script.close();
        QCOMPARE(fugue({"node", "test", "io.example.greeter/greeter"}, cwd).firstCode(), QString("node_bad_output"));
    }

    void notANodeFolderIsRefused() {
        const QString cwd = work("none");
        QCOMPARE(fugue({"node", "test", cwd}, cwd).firstCode(), QString("file_not_found"));
    }

    void theExamplePackPassesItsTests() {
        const QString pack = QStringLiteral(FUGUE_SOURCE_DIR "/examples/packs/io.github.bmaltais.fugue-example");
        const Outcome tested = fugue({"node", "test", pack}, work("example"));
        QVERIFY2(tested.exitCode == 0, tested.out.constData());
        // The persistent node's program lives across both cases.
        QCOMPARE(tested.json().value("data").toObject().value("tested").toInt(), 4);
    }
};

QTEST_GUILESS_MAIN(TestCliAuthoring)
#include "test_cli_authoring.moc"
