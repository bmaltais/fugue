// New templates without a release (spec 11.10, #699): the feed store (engine/template_feed.h) against a stand-in server, and
// what TemplateLibrary does with the folder it fetches.
//
// Seam: TemplateFeed over a FakeHttpServer on loopback and scratch folders, then TemplateLibrary/standardRoots over the result.
// Nothing reaches GitHub, and every folder is the test's own (XDG_* point into a temporary folder).

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "../engine/fake_feed.h"
#include "engine/registry.h"
#include "engine/settings.h"
#include "engine/template_feed.h"
#include "engine/templates.h"
#include "nodes/builtin.h"

using namespace fugue;
using fugue::test::FakeHttpServer;
using fugue::test::FakeRepo;

namespace {

QByteArray flow(const QString &nodeType = QStringLiteral("fugue/text")) {
    const QJsonObject node{{"type", nodeType}, {"version", "1.0.0"}, {"inputs", QJsonObject{{"text", "a fox"}}}};
    return QJsonDocument(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", QJsonObject{{"prompt", node}}}}}}).toJson();
}

QByteArray about(const QString &title) { return QJsonDocument(QJsonObject{{"title", title}}).toJson(); }

QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

FakeRepo repo() {
    FakeRepo r;
    r.files = {{"categories.json", "{\"categories\": []}"},
               {"alpha/template.json", about("Alpha")},
               {"alpha/workflow.fugue", flow()},
               {"beta/template.json", about("Beta")},
               {"beta/workflow.fugue", flow()}};
    return r;
}

} // namespace

class TestTemplateFeed : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    FakeHttpServer m_server;
    QNetworkAccessManager m_network;
    int m_run = 0;

    // A fresh data, state and config folder for each test.
    void freshHome() {
        const QString root = m_dir.filePath(QString::number(++m_run));
        qputenv("XDG_DATA_HOME", (root + "/data").toUtf8());
        qputenv("XDG_STATE_HOME", (root + "/state").toUtf8());
        qputenv("XDG_CONFIG_HOME", (root + "/config").toUtf8());
        qunsetenv("FUGUE_TEMPLATES");
        qputenv("FUGUE_TEMPLATE_FEED", m_server.origin());
        m_server.clearRequests();
    }

    FeedResult check(TemplateFeed &feed) {
        QSignalSpy spy(&feed, &TemplateFeed::finished);
        feed.check();
        if (spy.isEmpty())
            spy.wait(10000);
        return spy.isEmpty() ? FeedResult{} : qvariant_cast<FeedResult>(spy.first().first());
    }

    int rawRequests() const {
        int count = 0;
        for (const fugue::test::HttpRequest &request : m_server.requests())
            count += request.path.startsWith("/raw/");
        return count;
    }
    int treeRequests() const {
        int count = 0;
        for (const fugue::test::HttpRequest &request : m_server.requests())
            count += request.path.startsWith("/git/trees/");
        return count;
    }

    // The repo's files as a bundled root, with `changed` written differently.
    QString bundledLike(const FakeRepo &r, const QString &changed = {}) {
        const QString root = m_dir.filePath("bundled-" + QString::number(m_run));
        QDir(root).removeRecursively();
        for (auto it = r.files.cbegin(); it != r.files.cend(); ++it)
            write(root + "/" + it.key(), it.key() == changed ? QByteArray("not the same") : it.value());
        return root;
    }

private slots:
    void initTestCase() {
        qRegisterMetaType<FeedResult>();
        QVERIFY(m_server.listen());
    }

    void firstUpdateStartsFromTheBundledTemplates() {
        freshHome();
        const FakeRepo r = repo();
        r.serve(m_server);
        QNetworkAccessManager network;
        TemplateFeed feed(&network);
        feed.setBundledRoot(bundledLike(r, "beta/template.json"));
        const FeedResult result = check(feed);
        QCOMPARE(result.outcome, FeedResult::Outcome::Updated);
        QCOMPARE(result.files, 5);
        QCOMPARE(result.downloaded, 1);
        QCOMPARE(rawRequests(), 1);
        QCOMPARE(TemplateFeed::currentSha(), QString::fromLatin1(r.commit()));
        QCOMPARE(read(TemplateFeed::currentRoot() + "/beta/template.json"), r.files.value("beta/template.json"));
        QCOMPARE(read(TemplateFeed::currentRoot() + "/alpha/workflow.fugue"), r.files.value("alpha/workflow.fugue"));
        QVERIFY(TemplateFeed::status().error.isEmpty());
        QVERIFY(!TemplateFeed::due());
        QVERIFY(TemplateFeed::due(QDateTime::currentDateTimeUtc().addSecs(7 * 3600)));
    }

    void aNewShaWithOneChangedFileDownloadsOnlyThatFile() {
        freshHome();
        FakeRepo r = repo();
        r.serve(m_server);
        TemplateFeed feed(&m_network);
        feed.setBundledRoot(bundledLike(r));
        QCOMPARE(check(feed).outcome, FeedResult::Outcome::Updated);
        const QString first = TemplateFeed::currentSha();
        QCOMPARE(rawRequests(), 0);

        r.files["alpha/template.json"] = about("Alpha, better");
        r.serve(m_server);
        m_server.clearRequests();
        const FeedResult result = check(feed);
        QCOMPARE(result.outcome, FeedResult::Outcome::Updated);
        QCOMPARE(result.downloaded, 1);
        QCOMPARE(rawRequests(), 1);
        QCOMPARE(m_server.requests().last().path.startsWith("/raw/" + r.commit() + "/alpha/template.json"), true);
        QCOMPARE(result.previous, first);
        QCOMPARE(TemplateFeed::currentSha(), QString::fromLatin1(r.commit()));
        QVERIFY(!QFileInfo(TemplateFeed::storeDir() + "/" + first).exists()); // the older copy is gone
        QCOMPARE(read(TemplateFeed::currentRoot() + "/alpha/template.json"), about("Alpha, better"));
    }

    void theSameShaAsksNothingMore() {
        freshHome();
        const FakeRepo r = repo();
        r.serve(m_server);
        TemplateFeed feed(&m_network);
        feed.setBundledRoot(bundledLike(r));
        QCOMPARE(check(feed).outcome, FeedResult::Outcome::Updated);
        m_server.clearRequests();
        QCOMPARE(check(feed).outcome, FeedResult::Outcome::Current);
        QCOMPARE(m_server.requests().size(), 1); // the sha only
        QCOMPARE(treeRequests(), 0);
        QCOMPARE(m_server.requests().first().headers.value("accept"), QByteArray("application/vnd.github.sha"));
    }

    void aFileThatIsNotTheTreesIsRefusedAndTheOldCopyKept() {
        freshHome();
        FakeRepo r = repo();
        r.serve(m_server);
        TemplateFeed feed(&m_network);
        feed.setBundledRoot(bundledLike(r));
        QCOMPARE(check(feed).outcome, FeedResult::Outcome::Updated);
        const QString before = TemplateFeed::currentSha();

        r.files["alpha/template.json"] = about("Alpha, better");
        r.served["alpha/template.json"] = about("Not what the tree says");
        r.serve(m_server);
        const FeedResult result = check(feed);
        QCOMPARE(result.outcome, FeedResult::Outcome::Failed);
        QVERIFY(result.error.contains("refused"));
        QCOMPARE(TemplateFeed::currentSha(), before);
        QCOMPARE(read(TemplateFeed::currentRoot() + "/alpha/template.json"), about("Alpha"));
        QCOMPARE(QDir(TemplateFeed::storeDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).size(), 1); // no staging left
        QCOMPARE(TemplateFeed::status().error, result.error);
        QVERIFY(!TemplateFeed::due()); // a failed try is a try: no hammering
    }

    void aTreeThatIsNotPlainFilesOrIsCutShortIsRefused_data() {
        QTest::addColumn<QString>("entry");
        QTest::newRow("symlink") << R"({"path": "alpha/link", "mode": "120000", "type": "blob", "sha": "0123456789012345678901234567890123456789", "size": 3})";
        QTest::newRow("submodule") << R"({"path": "sub", "mode": "160000", "type": "commit", "sha": "0123456789012345678901234567890123456789"})";
        QTest::newRow("outside") << R"({"path": "../escape", "mode": "100644", "type": "blob", "sha": "0123456789012345678901234567890123456789", "size": 3})";
        QTest::newRow("windows path") << R"({"path": "a\\b", "mode": "100644", "type": "blob", "sha": "0123456789012345678901234567890123456789", "size": 3})";
        QTest::newRow("truncated") << QString();
    }
    void aTreeThatIsNotPlainFilesOrIsCutShortIsRefused() {
        QFETCH(QString, entry);
        freshHome();
        FakeRepo r = repo();
        if (entry.isEmpty())
            r.truncated = true;
        else
            r.extra.append(QJsonDocument::fromJson(entry.toUtf8()).object());
        r.serve(m_server);
        TemplateFeed feed(&m_network);
        feed.setBundledRoot(bundledLike(r));
        const FeedResult result = check(feed);
        QCOMPARE(result.outcome, FeedResult::Outcome::Failed);
        QVERIFY(TemplateFeed::currentRoot().isEmpty());
        QCOMPARE(rawRequests(), 0);
    }

    void anUnopenableOnlineTemplateLeavesTheBundledOneOfTheSameId() {
        freshHome();
        QTemporaryDir own, online, bundled;
        write(online.path() + "/alpha/template.json", about("Alpha online"));
        write(online.path() + "/alpha/workflow.fugue", flow());
        write(online.path() + "/beta/template.json", about("Beta online"));
        write(online.path() + "/beta/workflow.fugue", flow("fugue/a-node-from-the-future"));
        write(online.path() + "/gamma/template.json", about("Gamma online"));
        write(online.path() + "/gamma/workflow.fugue", flow("fugue/a-node-from-the-future"));
        write(bundled.path() + "/alpha/template.json", about("Alpha bundled"));
        write(bundled.path() + "/alpha/workflow.fugue", flow());
        write(bundled.path() + "/beta/template.json", about("Beta bundled"));
        write(bundled.path() + "/beta/workflow.fugue", flow());
        write(own.path() + "/alpha/template.json", about("Alpha own"));
        write(own.path() + "/alpha/workflow.fugue", flow());
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);

        const TemplateLibrary library({{own.path(), true}, TemplateRoot{online.path(), false, false, true}, {bundled.path(), false}}, &registry);
        QCOMPARE(library.find("alpha")->title, QString("Alpha own"));
        QCOMPARE(library.find("beta")->title, QString("Beta bundled")); // the online one needs a newer Fugue
        QVERIFY(!library.find("beta")->online);
        QVERIFY(!library.find("gamma")); // an online-only one that does not open is not listed

        const TemplateLibrary withoutOwn({TemplateRoot{online.path(), false, false, true}, {bundled.path(), false}}, &registry);
        QCOMPARE(withoutOwn.find("alpha")->title, QString("Alpha online"));
        QVERIFY(withoutOwn.find("alpha")->online);
    }

    void theSwitchAndTheEnvironmentDecideWhetherTheFetchedFolderIsARoot() {
        freshHome();
        const FakeRepo r = repo();
        r.serve(m_server);
        TemplateFeed feed(&m_network);
        feed.setBundledRoot(bundledLike(r));
        QCOMPARE(check(feed).outcome, FeedResult::Outcome::Updated);
        const auto hasOnline = [] {
            for (const TemplateRoot &root : TemplateLibrary::standardRoots())
                if (root.online)
                    return true;
            return false;
        };
        QVERIFY(Settings().fetchTemplates);
        QVERIFY(hasOnline());
        QVERIFY(!hasErrors(changeSetting(settingsPath(), "fetch_templates", false)));
        QVERIFY(!hasOnline());
        QVERIFY(!hasErrors(changeSetting(settingsPath(), "fetch_templates", true)));
        QVERIFY(hasOnline());
        qputenv("FUGUE_TEMPLATES", m_dir.path().toUtf8());
        QVERIFY(TemplateFeed::disabledByEnvironment());
        QVERIFY(!hasOnline());
        qunsetenv("FUGUE_TEMPLATES");
    }
};

QTEST_MAIN(TestTemplateFeed)
#include "test_template_feed.moc"
