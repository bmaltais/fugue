// The cache manager (spec 10.6, #141): what each kind of cache holds, clearing with filters and
// a dry run, the automatic limits, and what is never touched.
//
// Seam: CacheManager over folders in a temporary directory (never the owner's ~/.cache/fugue),
// with a fake clock. Results are made with the real ResultCache, run folders with RunFolder and
// the queue with Queue, so the manager reads what the engine writes.

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <unistd.h>

#include "engine/cache_manager.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/queue.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"
#include "engine/settings.h"

using namespace fugue;

class TestCacheManager : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QDateTime m_now;
    int m_count = 0;

    struct World {
        CacheFolders folders;
        QString nodes;
        std::unique_ptr<FileHashes> hashes;
        std::unique_ptr<ResultCache> cache;
        std::unique_ptr<CacheManager> manager;
    };

    World world() {
        World w;
        const QString base = m_dir.filePath(QString::number(++m_count));
        w.folders.results = base + "/results";
        w.folders.thumbnails = base + "/thumbnails";
        w.folders.runs = base + "/runs";
        w.folders.queue = base + "/queue";
        w.folders.lock = base + "/engine.lock";
        w.nodes = base + "/nodes";
        w.hashes = std::make_unique<FileHashes>(base + "/hashes.json");
        ResultCache::Options options;
        options.clock = [this] { return m_now; };
        w.cache = std::make_unique<ResultCache>(w.folders.results, options);
        w.manager = std::make_unique<CacheManager>(w.folders, [this] { return m_now; });
        return w;
    }

    static QString key(int n) { return sha256(QByteArray::number(n)); }

    void storeData(World &w, int n, const QString &node, int bytes, const QString &workflow = "/w/a.fugue") {
        const QString dir = w.nodes + "/" + QString::number(n) + node;
        QDir().mkpath(dir);
        const QString path = dir + "/latent.bin";
        const QByteArray data = QByteArray(bytes, char('a' + n % 26)) + QByteArray::number(n);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(data);
        file.close();
        const Values outputs{{"latent", QJsonObject{{"data", "fugue/latent"}, {"path", path}, {"hash", sha256(data)}}}};
        QVERIFY(w.cache->store(key(n), "fugue/fake", outputs, dir, UsedBy{workflow, node}, *w.hashes).ok());
    }

    void thumbnail(const World &w, const QString &name, int bytes, const QDateTime &changed) {
        QDir().mkpath(w.folders.thumbnails);
        QFile file(w.folders.thumbnails + "/" + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(bytes, 'x'));
        file.flush();
        file.setFileTime(changed, QFileDevice::FileModificationTime);
    }

    // A run folder with a file of its own, ended `daysAgo` days before now, or never ended.
    void run(const World &w, const QString &id, const QString &status, double daysAgo, const QString &workflow = {}) {
        const auto folder = RunFolder::create(w.folders.runs, id);
        QVERIFY(folder.ok());
        const QDateTime when = m_now.addSecs(-qint64(daysAgo * 86400));
        QJsonObject state{{"status", status}, {"started", isoTime(when)}};
        if (!workflow.isEmpty())
            state.insert("workflow", QJsonObject{{"path", workflow}});
        if (status != "running" && status != "queued")
            state.insert("ended", isoTime(when));
        QVERIFY(folder.value().writeState(state));
        QFile file(folder.value().path() + "/scratch.bin");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(1000, 'r'));
    }

    static const KindOutcome &outcome(const ClearReport &report, CacheKind kind) {
        for (const KindOutcome &each : report.kinds)
            if (each.kind == kind)
                return each;
        qFatal("no outcome for the kind");
    }

private slots:
    void init() { m_now = QDateTime(QDate(2026, 11, 21), QTime(12, 0), QTimeZone::UTC); }

    void statusMeasuresEachKindAndTheResultsPerWorkflowAndNode() {
        World w = world();
        storeData(w, 1, "a", 2000, "/w/one.fugue");
        storeData(w, 2, "b", 1000, "/w/one.fugue");
        storeData(w, 3, "a", 500, "/w/two.fugue");
        thumbnail(w, "x-512.png", 300, m_now.addDays(-3));
        thumbnail(w, "y-512.png", 200, m_now.addDays(-1));
        run(w, "r1", "ok", 2);

        const CacheStatus status = w.manager->status();
        const KindUsage *results = status.usage(CacheKind::Results);
        QVERIFY(results);
        QCOMPARE(results->items, 3);
        QCOMPARE(results->files, 3);
        QVERIFY(results->bytes > 3500);
        QCOMPARE(status.usage(CacheKind::Thumbnails)->bytes, 500);
        QCOMPARE(status.usage(CacheKind::Thumbnails)->files, 2);
        QCOMPARE(status.usage(CacheKind::Thumbnails)->oldest, m_now.addDays(-3));
        QCOMPARE(status.usage(CacheKind::Runs)->items, 1);
        QVERIFY(status.usage(CacheKind::Runs)->bytes >= 1000);

        QCOMPARE(status.workflows.size(), 2);
        QCOMPARE(status.workflows.at(0).workflow, QStringLiteral("/w/one.fugue")); // the largest first
        QCOMPARE(status.workflows.at(0).entries, 2);
        QCOMPARE(status.workflows.at(0).nodes.size(), 2);
        QCOMPARE(status.workflows.at(0).nodes.at(0).node, QStringLiteral("a"));
        QVERIFY(status.toJson().value("kinds").toArray().size() == 3);
    }

    void aRunsHardLinkedFilesCountForNothing() {
        World w = world();
        storeData(w, 1, "a", 2000);
        // The store moved the node's file into blobs/ and left a link in the node's folder:
        // taking a run's copy away frees nothing, so the run is not charged for it.
        const auto folder = RunFolder::create(w.folders.runs, "r1");
        QVERIFY(folder.ok());
        QDir().mkpath(folder.value().path() + "/nodes/a");
        QVERIFY(::link(QFile::encodeName(w.nodes + "/1a/latent.bin").constData(),
                       QFile::encodeName(folder.value().path() + "/nodes/a/latent.bin").constData()) == 0);
        const KindUsage *runs = w.manager->status().usage(CacheKind::Runs);
        QCOMPARE(runs->bytes, 0);
        QCOMPARE(runs->files, 1);
    }

    void clearingThumbnailsKeepsNothingButAPartFile() {
        World w = world();
        thumbnail(w, "x-512.png", 300, m_now.addDays(-30));
        thumbnail(w, "y-512.png", 200, m_now.addDays(-1));
        thumbnail(w, "z-512.png.part", 100, m_now);
        ClearRequest request;
        request.kinds = {CacheKind::Thumbnails};
        request.olderThanSeconds = 7 * 86400;
        request.dryRun = true;
        auto dry = w.manager->clear(request);
        QVERIFY(dry.ok());
        QCOMPARE(outcome(dry.value(), CacheKind::Thumbnails).removed, 1);
        QVERIFY(QFile::exists(w.folders.thumbnails + "/x-512.png")); // a dry run removes nothing
        request.dryRun = false;
        auto done = w.manager->clear(request);
        QCOMPARE(outcome(done.value(), CacheKind::Thumbnails).bytes, 300);
        QVERIFY(!QFile::exists(w.folders.thumbnails + "/x-512.png"));
        QVERIFY(QFile::exists(w.folders.thumbnails + "/y-512.png"));
        request.olderThanSeconds = 0;
        QCOMPARE(outcome(w.manager->clear(request).value(), CacheKind::Thumbnails).removed, 1);
        QVERIFY(QFile::exists(w.folders.thumbnails + "/z-512.png.part"));
    }

    void clearingResultsFollowsWorkflowNodeAndAgeAndSparesFrozenOnes() {
        World w = world();
        storeData(w, 1, "a", 1000, "/w/one.fugue");
        storeData(w, 2, "b", 1000, "/w/one.fugue");
        storeData(w, 3, "a", 1000, "/w/two.fugue");
        storeData(w, 4, "c", 1000, "/w/two.fugue");
        QVERIFY(w.cache->freeze(key(4)));

        ClearRequest request;
        request.workflow = "/w/one.fugue";
        request.node = "a";
        const Result<ClearReport> one = w.manager->clear(request);
        QVERIFY(one.ok());
        QCOMPARE(outcome(one.value(), CacheKind::Results).removed, 1);
        QVERIFY(!w.cache->contains(key(1)));
        QVERIFY(w.cache->contains(key(2)));
        QVERIFY(w.cache->contains(key(3)));

        request = {};
        request.dryRun = true;
        const ClearReport all = w.manager->clear(request).value();
        QCOMPARE(outcome(all, CacheKind::Results).removed, 2);
        QCOMPARE(outcome(all, CacheKind::Results).kept, 1); // the frozen one
        QVERIFY(outcome(all, CacheKind::Results).bytes >= 2000);
        QVERIFY(w.cache->contains(key(2)));
        request.dryRun = false;
        w.manager->clear(request);
        QVERIFY(!w.cache->contains(key(2)));
        QVERIFY(!w.cache->contains(key(3)));
        QVERIFY(w.cache->contains(key(4)));
    }

    void olderThanUsesTheLastUseOfAResult() {
        World w = world();
        m_now = m_now.addDays(-40);
        storeData(w, 1, "a", 1000);
        m_now = m_now.addDays(40);
        storeData(w, 2, "a", 1000);
        ClearRequest request;
        request.olderThanSeconds = 30 * 86400;
        QCOMPARE(outcome(w.manager->clear(request).value(), CacheKind::Results).removed, 1);
        QVERIFY(!w.cache->contains(key(1)));
        QVERIFY(w.cache->contains(key(2)));
    }

    void aRunThatIsGoingHoldsTheResultsAndNeverLosesItsFolder() {
        World w = world();
        storeData(w, 1, "a", 1000);
        run(w, "going", "running", 0.1);
        run(w, "waiting", "queued", 3);
        run(w, "done", "ok", 3);
        run(w, "stuck", "running", 5); // never ended and not queued: abandoned
        QVERIFY(Queue(w.folders.queue).enqueue("waiting", "/w/a.fugue", false));

        Lock engine(w.folders.lock);
        QVERIFY(engine.tryAcquire()); // a run holds the engine
        ClearRequest request;
        request.kinds = allCacheKinds();
        const ClearReport busy = w.manager->clear(request).value();
        QVERIFY(!outcome(busy, CacheKind::Results).skipped.isEmpty());
        QVERIFY(w.cache->contains(key(1)));
        QCOMPARE(outcome(busy, CacheKind::Runs).removed, 2); // done, stuck
        QCOMPARE(outcome(busy, CacheKind::Runs).kept, 2);    // going, waiting
        QVERIFY(QDir(w.folders.runs + "/going").exists());
        QVERIFY(QDir(w.folders.runs + "/waiting").exists());
        QVERIFY(!QDir(w.folders.runs + "/done").exists());
        QVERIFY(!QDir(w.folders.runs + "/stuck").exists());
        QCOMPARE(w.manager->activeRuns(), (QStringList{"going", "waiting"}));

        engine.release();
        QVERIFY(outcome(w.manager->clear(request).value(), CacheKind::Results).skipped.isEmpty());
        QVERIFY(!w.cache->contains(key(1)));
    }

    void runsCanBeClearedPerWorkflow() {
        World w = world();
        run(w, "a1", "ok", 1, "/w/one.fugue");
        run(w, "b1", "failed", 1, "/w/two.fugue");
        ClearRequest request;
        request.kinds = {CacheKind::Runs};
        request.workflow = "/w/two.fugue";
        QCOMPARE(outcome(w.manager->clear(request).value(), CacheKind::Runs).removed, 1);
        QVERIFY(QDir(w.folders.runs + "/a1").exists());
    }

    void requestsThatMakeNoSenseAreRefused() {
        World w = world();
        ClearRequest request;
        request.kinds = {CacheKind::Thumbnails};
        request.workflow = "/w/one.fugue";
        QCOMPARE(w.manager->clear(request).errors().first().code, QStringLiteral("usage"));
        request = {};
        request.kinds = {CacheKind::Runs};
        request.node = "a";
        QCOMPARE(w.manager->clear(request).errors().first().code, QStringLiteral("usage"));
    }

    void cleanupKeepsEachKindWithinItsLimit() {
        World w = world();
        for (int n = 1; n <= 4; ++n) {
            m_now = m_now.addSecs(60);
            storeData(w, n, "a", 1000);
        }
        thumbnail(w, "old-512.png", 400, m_now.addDays(-10)); // not shown for a week
        thumbnail(w, "a-512.png", 400, m_now.addDays(-3));
        thumbnail(w, "b-512.png", 400, m_now.addDays(-2));
        thumbnail(w, "c-512.png", 400, m_now.addDays(-1));
        for (int n = 0; n < 5; ++n)
            run(w, QStringLiteral("old%1").arg(n), "ok", 10 + n);
        run(w, "recent", "ok", 1);
        run(w, "going", "running", 0.5);

        CachePolicy policy;
        policy.resultsBytes = 2500; // room for two of the four
        policy.thumbnailsBytes = 900;
        policy.runsKept = 2;
        const ClearReport report = w.manager->cleanup(policy);
        QCOMPARE(outcome(report, CacheKind::Results).removed, 2);
        QVERIFY(!w.cache->contains(key(1)));
        QVERIFY(!w.cache->contains(key(2))); // the least recently used went
        QVERIFY(w.cache->contains(key(4)));
        QVERIFY(!QFile::exists(w.folders.thumbnails + "/old-512.png"));
        QVERIFY(!QFile::exists(w.folders.thumbnails + "/a-512.png")); // over the limit: the oldest
        QVERIFY(QFile::exists(w.folders.thumbnails + "/b-512.png"));
        QVERIFY(QFile::exists(w.folders.thumbnails + "/c-512.png"));
        // The two newest ended runs stay whatever their age, and the one going.
        QVERIFY(QDir(w.folders.runs + "/recent").exists());
        QVERIFY(QDir(w.folders.runs + "/old0").exists());
        QVERIFY(QDir(w.folders.runs + "/going").exists());
        QVERIFY(!QDir(w.folders.runs + "/old1").exists());
        QVERIFY(!QDir(w.folders.runs + "/old4").exists());
    }

    void cleanupNeverTouchesResultsOrPreviewsWhileARunHoldsTheEngine() {
        World w = world();
        storeData(w, 1, "a", 1000);
        thumbnail(w, "old-512.png", 400, m_now.addDays(-10));
        Lock engine(w.folders.lock);
        QVERIFY(engine.tryAcquire());
        CachePolicy policy;
        policy.resultsBytes = 10;
        const ClearReport report = w.manager->cleanup(policy);
        QVERIFY(!outcome(report, CacheKind::Results).skipped.isEmpty());
        QVERIFY(!outcome(report, CacheKind::Thumbnails).skipped.isEmpty());
        QVERIFY(w.cache->contains(key(1)));
        QVERIFY(QFile::exists(w.folders.thumbnails + "/old-512.png"));
    }

    void noLimitMeansNoEviction() {
        World w = world();
        storeData(w, 1, "a", 1000);
        CachePolicy policy;
        policy.resultsBytes = 0;
        policy.thumbnailsBytes = 0;
        w.manager->cleanup(policy);
        QVERIFY(w.cache->contains(key(1)));
    }

    void thePolicyComesFromTheSettings() {
        Settings settings;
        settings.cacheResultsGb = 2.5;
        settings.cacheThumbnailsGb = 0;
        settings.runsKept = 3;
        const CachePolicy policy = CachePolicy::from(settings);
        QCOMPARE(policy.resultsBytes, qint64(2500000000));
        QCOMPARE(policy.thumbnailsBytes, qint64(0));
        QCOMPARE(policy.runsKept, 3);
    }
};

QTEST_GUILESS_MAIN(TestCacheManager)
#include "test_cache_manager.moc"
