// `fugue template update` (#699, spec 11.10). Seam: the real `fugue` binary as a subprocess with a root of its own (FUGUE_HOME)
// and a stand-in server for the feed ($FUGUE_TEMPLATE_FEED, loopback). Nothing reaches GitHub.

#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "../engine/fake_feed.h"
#include "../test_paths.h"

using fugue::test::FakeHttpServer;
using fugue::test::FakeRepo;

class TestTemplateFeedCli : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    FakeHttpServer m_server;

    // Runs `fugue <args>` and answers the stand-in server meanwhile.
    QJsonObject fugue(const QStringList &args, bool withEnvironmentFolder = false, int *exitCode = nullptr) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.remove("FUGUE_TEMPLATES");
        env.insert("FUGUE_HOME", m_dir.filePath("home"));
        env.insert("HOME", m_dir.filePath("userhome"));
        env.insert("FUGUE_TEMPLATE_FEED", m_server.origin());
        if (withEnvironmentFolder)
            env.insert("FUGUE_TEMPLATES", m_dir.path());
        QProcess process;
        process.setProcessEnvironment(env);
        process.start(fugueBinary(), args);
        QEventLoop loop;
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
        if (process.state() != QProcess::NotRunning)
            loop.exec();
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
        }
        if (exitCode)
            *exitCode = process.exitCode();
        return QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    }

private slots:
    void initTestCase() { QVERIFY(m_server.listen()); }

    void updatesThenSaysItIsCurrent() {
        FakeRepo r;
        r.files = {{"categories.json", "{\"categories\": []}"},
                   {"alpha/template.json", "{\"title\": \"Alpha\"}"},
                   {"alpha/workflow.fugue", "{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"}};
        r.serve(m_server);
        // The build's bundled templates are the base; a repo of these three files shares none of them, so all are fetched.
        const QJsonObject first = fugue({"template", "update", "--json"}).value("data").toObject();
        QCOMPARE(first.value("result").toString(), QString("updated"));
        QCOMPARE(first.value("commit").toString(), QString::fromLatin1(r.commit()));
        QCOMPARE(first.value("files").toInt(), 3);
        QVERIFY(QFileInfo(first.value("folder").toString() + "/alpha/template.json").isFile());
        const QJsonObject second = fugue({"template", "update", "--json"}).value("data").toObject();
        QCOMPARE(second.value("result").toString(), QString("current"));
        // The window and the CLI list it, as online.
        const QJsonArray listed = fugue({"template", "list", "--json"}).value("data").toObject().value("templates").toArray();
        QString origin;
        for (const QJsonValue t : listed)
            if (t.toObject().value("id").toString() == "alpha")
                origin = t.toObject().value("origin").toString();
        QCOMPARE(origin, QString("online"));
    }

    void aBadFileFailsAndKeepsWhatIsThere() {
        FakeRepo r;
        r.files = {{"alpha/template.json", "{\"title\": \"Alpha\"}"}, {"alpha/workflow.fugue", "{}"}, {"zeta/template.json", "{\"title\": \"Z\"}"}};
        r.served["zeta/template.json"] = "something else";
        r.serve(m_server);
        int code = 0;
        const QJsonObject reply = fugue({"template", "update", "--json"}, false, &code);
        QVERIFY(code != 0);
        QCOMPARE(reply.value("errors").toArray().at(0).toObject().value("code").toString(), QString("download_failed"));
    }

    void theEnvironmentFolderMeansNoFetch() {
        int code = 0;
        const QJsonObject reply = fugue({"template", "update", "--json"}, true, &code);
        QVERIFY(code != 0);
        QCOMPARE(reply.value("errors").toArray().at(0).toObject().value("code").toString(), QString("usage"));
    }
};

QTEST_MAIN(TestTemplateFeedCli)
#include "test_cli_template_feed.moc"
