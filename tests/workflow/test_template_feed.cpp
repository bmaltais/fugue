// New templates without a release (spec 11.10, #699): the feed store (engine/template_feed.h) against a stand-in server, and
// what TemplateLibrary does with the folder it fetches.
//
// Seam: TemplateFeed over a FakeHttpServer on loopback and scratch folders, then TemplateLibrary/standardRoots over the result.
// Nothing reaches GitHub, and every folder is the test's own (XDG_* point into a temporary folder).

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "../engine/fake_feed.h"
#include "engine/registry.h"
#include "engine/settings.h"
#include "engine/template_feed.h"
#include "apps/app_ref.h"
#include "engine/template_feed_driver.h"
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
        QVERIFY(QFileInfo(TemplateFeed::storeDir() + "/" + first).exists()); // the previous copy stays, a scan may still read it
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
        QByteArray other = about("Alpha, better");
        other.replace("better", "bitter"); // as long as the tree says, but not the same bytes
        r.served["alpha/template.json"] = other;
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

    void rateLimitsAndRefusalsSayWhatTheyAre() {
        freshHome();
        TemplateFeed feed(&m_network);
        const auto answer = [&](int status, const QList<QByteArray> &headers) {
            m_server.on("GET", "/commits/main", [=](const fugue::test::HttpRequest &) {
                return fugue::test::HttpResponse{status, "{}", "application/json", false, -1, headers};
            });
            return check(feed);
        };
        FeedResult result = answer(403, {"x-ratelimit-remaining: 0"});
        QCOMPARE(result.outcome, FeedResult::Outcome::Failed);
        QVERIFY(result.error.contains("rate limit"));
        result = answer(429, {});
        QVERIFY(result.error.contains("rate limit"));
        result = answer(403, {"x-ratelimit-remaining: 12"});
        QVERIFY(result.error.contains("403"));
        QVERIFY(!result.error.contains("rate limit"));
    }

    void aFileLargerThanTheTreeSaysOrTheCapIsRefused() {
        freshHome();
        FakeRepo r = repo();
        r.served["beta/template.json"] = about("Beta, and a lot more bytes than the tree says it has");
        r.serve(m_server);
        TemplateFeed feed(&m_network);
        feed.setBundledRoot(m_dir.filePath("none")); // nothing to copy: every file is downloaded
        FeedResult result = check(feed);
        QCOMPARE(result.outcome, FeedResult::Outcome::Failed);
        QVERIFY2(result.error.contains("larger than"), qPrintable(result.error));
        QVERIFY(TemplateFeed::currentRoot().isEmpty());

        r = repo();
        r.serve(m_server);
        TemplateFeed::setLimitsForTest(8, 1 << 20, 5000);
        result = check(feed);
        QVERIFY2(result.error.contains("is over"), qPrintable(result.error));
        TemplateFeed::setLimitsForTest(1 << 20, 100, 5000);
        QVERIFY(check(feed).error.contains("over the size"));
        TemplateFeed::setLimitsForTest(1 << 20, 1 << 20, 2);
        QVERIFY(check(feed).error.contains("over the size"));
        TemplateFeed::resetLimitsForTest();
    }

    void unsafeNamesAndAFileThatIsAlsoAFolderAreRefused_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("absolute") << "/etc/passwd";
        QTest::newRow("empty part") << "alpha//x";
        QTest::newRow("colon") << "c:evil";
        QTest::newRow("a file and a folder") << "alpha";
    }
    void unsafeNamesAndAFileThatIsAlsoAFolderAreRefused() {
        QFETCH(QString, path);
        freshHome();
        FakeRepo r = repo();
        r.extra.append(QJsonObject{{"path", path}, {"mode", "100644"}, {"type", "blob"}, {"sha", "0123456789012345678901234567890123456789"}, {"size", 3}});
        r.serve(m_server);
        TemplateFeed feed(&m_network);
        QCOMPARE(check(feed).outcome, FeedResult::Outcome::Failed);
        QVERIFY(TemplateFeed::currentRoot().isEmpty());
        QCOMPARE(rawRequests(), 0);
    }

    void dotFilesAreLeftOutEvenALinkInADotFolder() {
        freshHome();
        FakeRepo r = repo();
        r.files[".github/ci.yml"] = "on: push";
        r.files[".gitignore"] = "x";
        r.extra.append(QJsonObject{{"path", ".github/link"}, {"mode", "120000"}, {"type", "blob"}, {"sha", "0123456789012345678901234567890123456789"}, {"size", 3}});
        r.serve(m_server);
        TemplateFeed feed(&m_network);
        feed.setBundledRoot(bundledLike(r));
        const FeedResult result = check(feed);
        QCOMPARE(result.outcome, FeedResult::Outcome::Updated);
        QCOMPARE(result.files, 5);
        QVERIFY(!QFileInfo::exists(TemplateFeed::currentRoot() + "/.github"));
        QVERIFY(!QFileInfo::exists(TemplateFeed::currentRoot() + "/.gitignore"));
    }

    void aBusyLockSkipsTheUpdateQuietly() {
        freshHome();
        const FakeRepo r = repo();
        r.serve(m_server);
        QDir().mkpath(TemplateFeed::storeDir());
        QLockFile held(TemplateFeed::storeDir() + "/.lock");
        QVERIFY(held.tryLock(0));
        TemplateFeed feed(&m_network);
        const FeedResult result = check(feed);
        QCOMPARE(result.outcome, FeedResult::Outcome::Busy);
        QCOMPARE(m_server.requests().size(), 0);
        QVERIFY(!TemplateFeed::status().checked.isValid()); // not a try
        held.unlock();
        QCOMPARE(check(feed).outcome, FeedResult::Outcome::Updated);
    }

    void thePreviousCopyIsKeptAndOlderOnesGo() {
        freshHome();
        FakeRepo r = repo();
        TemplateFeed feed(&m_network);
        feed.setBundledRoot(bundledLike(r));
        QStringList shas;
        for (int i = 0; i < 3; ++i) {
            r.files["alpha/template.json"] = about("Alpha " + QString::number(i));
            r.serve(m_server);
            QCOMPARE(check(feed).outcome, FeedResult::Outcome::Updated);
            shas << TemplateFeed::currentSha();
        }
        QVERIFY(!QFileInfo::exists(TemplateFeed::storeDir() + "/" + shas[0]));
        QVERIFY(QFileInfo::exists(TemplateFeed::storeDir() + "/" + shas[1]));
        QVERIFY(QFileInfo::exists(TemplateFeed::storeDir() + "/" + shas[2]));
    }

    void aFallbackSortsInTheFetchedCategoryOrderAndAnIdDeletedUpstreamIsHidden() {
        freshHome();
        QTemporaryDir online, bundled;
        const auto category = [](const QString &title, const QString &category) {
            return QJsonDocument(QJsonObject{{"title", title}, {"category", category}}).toJson();
        };
        write(online.path() + "/categories.json", R"({"categories": [{"id": "b", "title": "B"}, {"id": "a", "title": "A"}]})");
        write(online.path() + "/alpha/template.json", category("Alpha", "a"));
        write(online.path() + "/alpha/workflow.fugue", flow());
        write(online.path() + "/beta/template.json", category("Beta", "b"));
        write(online.path() + "/beta/workflow.fugue", flow("fugue/a-node-from-the-future"));
        write(bundled.path() + "/categories.json", R"({"categories": [{"id": "a", "title": "A"}, {"id": "b", "title": "B"}]})");
        write(bundled.path() + "/alpha/template.json", category("Alpha bundled", "a"));
        write(bundled.path() + "/alpha/workflow.fugue", flow());
        write(bundled.path() + "/beta/template.json", category("Beta bundled", "b"));
        write(bundled.path() + "/beta/workflow.fugue", flow());
        write(bundled.path() + "/gone/template.json", category("Gone", "a"));
        write(bundled.path() + "/gone/workflow.fugue", flow());
        // A template this release added that the snapshot (made before it) never heard of stays: only what was bundled when the
        // snapshot was made and is not in it was deleted upstream.
        write(bundled.path() + "/fresh/template.json", category("Fresh", "a"));
        write(bundled.path() + "/fresh/workflow.fugue", flow());
        write(online.path() + "/bundled-ids.json", R"({"bundled": ["alpha", "beta", "gone"]})");
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        const TemplateLibrary library({TemplateRoot{online.path(), false, false, true}, {bundled.path(), false}}, &registry);
        QStringList ids;
        for (const Template &t : library.list())
            ids << t.id;
        QCOMPARE(ids, (QStringList{"beta", "alpha", "fresh"})); // the fetched order (B, then A) holds for the bundled stand-in too; "gone" is hidden
        // Without a fetched snapshot nothing is hidden.
        const TemplateLibrary plain({{bundled.path(), false}}, &registry);
        QCOMPARE(plain.list().size(), 4);
    }

    void anOnlineTemplateFolderIsAnAppByItsIdNotItsPath() {
        freshHome();
        const FakeRepo r = repo();
        r.serve(m_server);
        TemplateFeed feed(&m_network);
        feed.setBundledRoot(bundledLike(r));
        QCOMPARE(check(feed).outcome, FeedResult::Outcome::Updated);
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        QString why;
        const auto app = resolveApp(TemplateFeed::currentRoot() + "/alpha", {}, &registry, &why); // no live roots at all: still the id
        QVERIFY2(app.has_value(), qPrintable(why));
        QCOMPARE(app->ref, QString("alpha"));
    }

    void theDriverFetchesAfterItsDelayOnASwitchAndAgain() {
        freshHome();
        const FakeRepo r = repo();
        r.serve(m_server);
        TemplateFeed feed(&m_network);
        feed.setBundledRoot(bundledLike(r));
        bool enabled = false;
        TemplateFeedDriver::Options options;
        options.enabled = [&] { return enabled; };
        options.allowed = [] { return true; };
        options.due = [] { return true; };
        options.startDelayMs = 20;
        options.recheckMs = 60;
        TemplateFeedDriver driver(&feed, options);
        QSignalSpy roots(&driver, &TemplateFeedDriver::rootsChanged);
        QSignalSpy checked(&driver, &TemplateFeedDriver::checked);
        driver.start();
        QTest::qWait(150);
        QCOMPARE(m_server.requests().size(), 0); // switched off: never asks

        enabled = true;
        driver.settingsChanged(); // the switch turned on: roots change at once and a check starts
        QCOMPARE(roots.size(), 1);
        QTRY_COMPARE(checked.size(), 1);
        QCOMPARE(roots.size(), 2); // the update is a root change too
        QTRY_VERIFY(checked.size() >= 2); // the 6-hourly timer, here every 60 ms: the sha only
        driver.settingsChanged(); // nothing moved
        QCOMPARE(roots.size(), 2);

        enabled = false;
        driver.settingsChanged();
        QCOMPARE(roots.size(), 3);
        const int asked = int(m_server.requests().size());
        QTest::qWait(150);
        QCOMPARE(int(m_server.requests().size()), asked);
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
        qputenv("FUGUE_HOME", m_dir.filePath("scratch-root").toUtf8()); // a scratch root has a store of its own, listed even in a dev build
        QVERIFY(hasOnline());
        QVERIFY(!hasErrors(changeSetting(settingsPath(), "fetch_templates", false)));
        QVERIFY(!hasOnline());
        QVERIFY(!hasErrors(changeSetting(settingsPath(), "fetch_templates", true)));
        QVERIFY(hasOnline());
        // A development build (the bundled folder is the build tree's link) never lists the shared store, so the checkout's own
        // templates are not hidden by the installed Fugue's fetched copy; a scratch root's own store is listed.
        qunsetenv("FUGUE_HOME");
        QVERIFY(TemplateLibrary::bundledIsBuildTree());
        QVERIFY(TemplateFeed::storeIsShared());
        QVERIFY(!hasOnline());
        qputenv("FUGUE_HOME", m_dir.filePath("scratch-root").toUtf8());
        QVERIFY(hasOnline());
        qputenv("FUGUE_TEMPLATES", m_dir.path().toUtf8());
        QVERIFY(TemplateFeed::disabledByEnvironment());
        QVERIFY(!hasOnline());
        qunsetenv("FUGUE_TEMPLATES");
        qunsetenv("FUGUE_HOME");
    }
};

QTEST_MAIN(TestTemplateFeed)
#include "test_template_feed.moc"
