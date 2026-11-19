// The result cache (spec 10): keys, the store of key records and content
// blobs, freeze, eviction and garbage collection.
//
// Seam: ResultCache's public interface over a temporary root, observed
// through what lookup() gives back and the files under the root. Time comes
// from a fake clock.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>

#include <vips/vips8>

#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/json.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"
#include "engine/vips.h"

using namespace fugue;

namespace {

bool isUnder(const QString &path, const QString &dir) {
    return path.startsWith(dir + QLatin1Char('/'));
}

quint64 inode(const QString &path) {
    struct stat info {};
    return ::stat(QFile::encodeName(path).constData(), &info) == 0 ? quint64(info.st_ino) : 0;
}

QByteArray contents(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class TestResultCache : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QDateTime m_now;
    int m_count = 0;

    // A fresh world per test function.
    struct World {
        QString root;
        QString nodes;
        std::unique_ptr<FileHashes> hashes;
        std::unique_ptr<ResultCache> cache;
    };

    World world(qint64 limit = ResultCache::defaultLimit) {
        World w;
        const QString base = m_dir.filePath(QString::number(++m_count));
        w.root = base + "/results";
        w.nodes = base + "/nodes";
        w.hashes = std::make_unique<FileHashes>(base + "/hashes.json");
        ResultCache::Options options;
        options.limitBytes = limit;
        options.clock = [this] { return m_now; };
        w.cache = std::make_unique<ResultCache>(w.root, options);
        return w;
    }

    // A node folder holding one data file, and the output that names it.
    Values dataOutput(const World &w, const QString &node, const QByteArray &bytes) {
        const QString dir = w.nodes + "/" + node;
        QDir().mkpath(dir);
        const QString path = dir + "/latent.bin";
        // What an earlier store left here is a read-only link to a blob.
        QFile::remove(path);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            qFatal("cannot write %s", qPrintable(path));
        file.close();
        return {{"latent", QJsonObject{{"data", "fugue/latent"}, {"path", path}, {"hash", sha256(bytes)}}}};
    }

    QString storeData(World &w, const QString &key, const QString &node, const QByteArray &bytes,
                      const QString &workflow = "/w/a.fugue") {
        const auto stored = w.cache->store(key, "fugue/fake", dataOutput(w, node, bytes), w.nodes + "/" + node,
                                           UsedBy{workflow, node}, *w.hashes);
        if (!stored)
            qFatal("store failed: %s", qPrintable(stored.error().message));
        return stored.value().value("latent").json().toObject().value("path").toString();
    }

    static QString key(int n) { return sha256(QByteArray::number(n)); }

private slots:
    void init() { m_now = QDateTime(QDate(2026, 11, 19), QTime(12, 0), QTimeZone::UTC); }

    // --- keys (10.1) ---

    void keyIsAHashThatEveryPartChanges() {
        KeyParts base;
        base.type = "fugue/blur";
        base.version = Version{1, 2, 0};
        base.pack = "fugue";
        base.inputs = QJsonObject{{"radius", 2}};

        const QString k = cacheKey(base);
        QVERIFY2(isSha256(k), qPrintable(k));
        QCOMPARE(cacheKey(base), k);

        QSet<QString> keys{k};
        auto differs = [&](std::function<void(KeyParts &)> change) {
            KeyParts changed = base;
            change(changed);
            const QString other = cacheKey(changed);
            const bool isNew = !keys.contains(other);
            keys.insert(other);
            return isNew;
        };
        QVERIFY(differs([](KeyParts &p) { p.type = "fugue/sharpen"; }));
        QVERIFY(differs([](KeyParts &p) { p.version = Version{1, 2, 1}; }));
        QVERIFY(differs([](KeyParts &p) { p.pack = "io.example.tools"; }));
        QVERIFY(differs([](KeyParts &p) { p.packVersion = "2.0.0"; }));
        QVERIFY(differs([](KeyParts &p) { p.inputs.insert("radius", 3); }));
        QVERIFY(differs([](KeyParts &p) { p.engine = "sdcpp"; }));
        QVERIFY(differs([](KeyParts &p) { p.engine = "sdcpp"; p.model = "sha256:abc"; }));
        QVERIFY(differs([](KeyParts &p) { p.run = "20261119-120000-000-abcd"; }));
    }

    void resourceIdentityIgnoresDisplayFields() {
        const QJsonObject handle{{"resource", "fugue/lora"}, {"hash", "sha256:aa"}, {"name", "detail.safetensors"}};
        QJsonObject renamed = handle;
        renamed.insert("name", "renamed.safetensors");
        renamed.insert("needs", QJsonObject{{"vram_mb", 4000}});
        QCOMPARE(resourceIdentity(renamed), resourceIdentity(handle));

        QJsonObject replaced = handle;
        replaced.insert("hash", "sha256:bb");
        QVERIFY(resourceIdentity(replaced) != resourceIdentity(handle));

        // Served by name: the name is the identity.
        const QJsonObject byName{{"resource", "fugue/image-model"}, {"name", "flux"}};
        const QJsonObject otherName{{"resource", "fugue/image-model"}, {"name", "sdxl"}};
        QVERIFY(resourceIdentity(byName) != resourceIdentity(otherName));
    }

    void aValuesIdentityIsItsContentNotItsPath() {
        const Value here(QJsonObject{{"data", "fugue/latent"}, {"path", "/a/x.bin"}, {"hash", "sha256:aa"}});
        const Value there(QJsonObject{{"data", "fugue/latent"}, {"path", "/b/y.bin"}, {"hash", "sha256:aa"}});
        const Value other(QJsonObject{{"data", "fugue/latent"}, {"path", "/a/x.bin"}, {"hash", "sha256:bb"}});
        QCOMPARE(here.identity(), there.identity());
        QVERIFY(here.identity() != other.identity());
    }

    // --- the store (10.2, 9.7) ---

    void aNodesFileMovesIntoTheStoreAndAHardLinkStays() {
        World w = world();
        const QByteArray bytes("latent bytes");
        const QString original = w.nodes + "/gen/latent.bin";
        const QString stored = storeData(w, key(1), "gen", bytes);

        const QString hex = sha256(bytes).mid(7);
        QCOMPARE(stored, w.root + "/blobs/" + hex + ".bin");
        QCOMPARE(contents(stored), bytes);
        QCOMPARE(contents(original), bytes);
        QVERIFY(inode(stored) != 0);
        QCOMPARE(inode(original), inode(stored));
        QVERIFY(QFile::exists(w.root + "/keys/" + key(1).mid(7) + ".json"));
    }

    void lookupGivesBackEveryKindOfValue() {
        World w = world();
        Values outputs = dataOutput(w, "n", "abc");
        outputs.insert("text", QJsonValue("hello"));
        outputs.insert("seed", QJsonValue(42));
        outputs.insert("flags", Value::list({Value(QJsonValue(true)), Value(QJsonValue("x"))}));
        QVERIFY(w.cache->store(key(1), "fugue/fake", outputs, w.nodes + "/n", UsedBy{"/w/a.fugue", "n"}, *w.hashes));

        QVERIFY(w.cache->contains(key(1)));
        QVERIFY(!w.cache->contains(key(2)));
        QVERIFY(!w.cache->lookup(key(2), UsedBy{}, *w.hashes));

        const auto found = w.cache->lookup(key(1), UsedBy{"/w/a.fugue", "n"}, *w.hashes);
        QVERIFY(found.has_value());
        QCOMPARE(found->value("text").text(), QStringLiteral("hello"));
        QCOMPARE(found->value("seed").number(), 42.0);
        QCOMPARE(found->value("flags").toJson(), QJsonValue(QJsonArray{true, "x"}));
        const QJsonObject latent = found->value("latent").json().toObject();
        QCOMPARE(latent.value("hash").toString(), sha256(QByteArray("abc")));
        QVERIFY(isUnder(latent.value("path").toString(), w.root + "/blobs"));
        QCOMPARE(contents(latent.value("path").toString()), QByteArray("abc"));
    }

    void anImageIsRecordedAsAMediaHandle() {
        QVERIFY(initVips());
        World w = world();
        const QString dir = w.nodes + "/img";
        const vips::VImage pixels = vips::VImage::black(8, 6).cast(VIPS_FORMAT_UCHAR);
        const auto made = MediaHandle::fromImage(pixels);
        QVERIFY(made.ok());
        const auto saved = made.value().saved(dir, "image", *w.hashes);
        QVERIFY(saved.ok());
        QVERIFY(w.cache->store(key(1), "fugue/fake", {{"image", saved.value()}}, dir, UsedBy{"", "img"}, *w.hashes));

        const auto found = w.cache->lookup(key(1), UsedBy{"", "img"}, *w.hashes);
        QVERIFY(found.has_value());
        const Value image = found->value("image");
        QVERIFY(image.isMedia());
        QCOMPARE(image.media().width(), 8);
        QCOMPARE(image.media().height(), 6);
        QCOMPARE(image.media().hash(), saved.value().hash());
        QCOMPARE(image.media().path(), w.root + "/blobs/" + saved.value().hash().mid(7) + ".png");
        QVERIFY(QFile::exists(dir + "/image.png"));
    }

    void theSameContentIsStoredOnce() {
        World w = world();
        const QString first = storeData(w, key(1), "a", "same");
        const QString second = storeData(w, key(2), "b", "same");
        QCOMPARE(second, first);
        QCOMPARE(QDir(w.root + "/blobs").entryList(QDir::Files).size(), 1);
        QCOMPARE(inode(w.nodes + "/b/latent.bin"), inode(first));
        QCOMPARE(w.cache->size(), qint64(4));
    }

    void aFileOutsideTheNodeFolderIsLeftWhereItIs() {
        World w = world();
        const QString outside = m_dir.filePath("user-file.bin");
        QFile file(outside);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("mine");
        file.close();
        const Values outputs{{"latent", QJsonObject{{"data", "fugue/latent"}, {"path", outside}, {"hash", sha256(QByteArray("mine"))}}}};
        const auto stored = w.cache->store(key(1), "fugue/load", outputs, w.nodes + "/load", UsedBy{"", "load"}, *w.hashes);
        QVERIFY(stored.ok());
        QCOMPARE(stored.value().value("latent").json().toObject().value("path").toString(), outside);
        QVERIFY(QDir(w.root + "/blobs").entryList(QDir::Files).isEmpty());
        QVERIFY(w.cache->lookup(key(1), UsedBy{"", "load"}, *w.hashes).has_value());

        // Once the file is gone, the record is a miss (a sink writes again, 10.2).
        QVERIFY(QFile::remove(outside));
        QVERIFY(!w.cache->lookup(key(1), UsedBy{"", "load"}, *w.hashes).has_value());
        QVERIFY(!w.cache->contains(key(1)));
    }

    void aRecordWhoseBlobIsGoneIsAMiss() {
        World w = world();
        const QString stored = storeData(w, key(1), "a", "bytes");
        QVERIFY(QFile::remove(stored));
        QVERIFY(!w.cache->peek(key(1), *w.hashes).has_value());
        QVERIFY(!w.cache->lookup(key(1), UsedBy{}, *w.hashes).has_value());
    }

    void theRecordSaysWhatUsedIt() {
        World w = world();
        storeData(w, key(1), "gen", "bytes", "/w/a.fugue");
        m_now = m_now.addSecs(60);
        QVERIFY(w.cache->lookup(key(1), UsedBy{"/w/b.fugue", "g7/gen"}, *w.hashes).has_value());
        QVERIFY(w.cache->lookup(key(1), UsedBy{"/w/b.fugue", "g7/gen"}, *w.hashes).has_value());

        const QList<CacheEntry> entries = w.cache->entries();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().key, key(1));
        QCOMPARE(entries.first().type, QStringLiteral("fugue/fake"));
        QCOMPARE(entries.first().size, qint64(5));
        QCOMPARE(entries.first().lastUsed, m_now);
        const QList<UsedBy> expected{UsedBy{"/w/a.fugue", "gen"}, UsedBy{"/w/b.fugue", "g7/gen"}};
        QCOMPARE(entries.first().usedBy, expected);
    }

    void peekDoesNotCountAsUse() {
        World w = world();
        storeData(w, key(1), "gen", "bytes");
        const QDateTime stored = m_now;
        m_now = m_now.addSecs(60);
        QVERIFY(w.cache->peek(key(1), *w.hashes).has_value());
        QCOMPARE(w.cache->entries().first().lastUsed, stored);
    }

    // --- eviction (10.5) ---

    void overTheLimitTheLeastRecentlyUsedGoFirst() {
        World w = world(25); // room for two 10-byte results
        storeData(w, key(1), "a", "aaaaaaaaaa");
        m_now = m_now.addSecs(1);
        storeData(w, key(2), "b", "bbbbbbbbbb");
        m_now = m_now.addSecs(1);
        storeData(w, key(3), "c", "cccccccccc");
        m_now = m_now.addSecs(1);
        // Using the oldest makes the second the least recently used.
        QVERIFY(w.cache->lookup(key(1), UsedBy{"/w/a.fugue", "a"}, *w.hashes).has_value());

        QCOMPARE(w.cache->size(), qint64(30));
        QCOMPARE(w.cache->evict(), QStringList{key(2)});
        QCOMPARE(w.cache->size(), qint64(20));
        QVERIFY(w.cache->contains(key(1)));
        QVERIFY(!w.cache->contains(key(2)));
        QVERIFY(w.cache->contains(key(3)));
        QVERIFY(w.cache->evict().isEmpty());
    }

    void aBlobTwoRecordsShareStaysWhileOneOfThemDoes() {
        World w = world(5);
        const QString blob = storeData(w, key(1), "a", "shared-ten");
        m_now = m_now.addSecs(1);
        storeData(w, key(2), "b", "shared-ten");
        // 10 bytes over a limit of 5: both records must go before the blob does.
        QCOMPARE(w.cache->evict(), (QStringList{key(1), key(2)}));
        QVERIFY(!QFile::exists(blob));
    }

    void aFrozenResultIsNeverEvicted() {
        World w = world(5);
        storeData(w, key(1), "a", "aaaaaaaaaa");
        m_now = m_now.addSecs(1);
        storeData(w, key(2), "b", "bbbbbbbbbb");
        QVERIFY(w.cache->freeze(key(1)));
        QVERIFY(w.cache->isFrozen(key(1)));
        QCOMPARE(w.cache->frozen(), QStringList{key(1)});
        QVERIFY(QFile::exists(w.root + "/frozen/" + key(1).mid(7)));

        QCOMPARE(w.cache->evict(), QStringList{key(2)});
        QVERIFY(w.cache->contains(key(1)));
        QVERIFY(w.cache->entries().first().frozen);

        w.cache->unfreeze(key(1));
        QVERIFY(!w.cache->isFrozen(key(1)));
        QCOMPARE(w.cache->evict(), QStringList{key(1)});
    }

    void onlyAnExistingResultCanBeFrozen() {
        World w = world();
        QVERIFY(!w.cache->freeze(key(9)));
        QVERIFY(w.cache->frozen().isEmpty());
    }

    // --- clear (10.5) ---

    void clearByWorkflowAndByNode() {
        World w = world();
        storeData(w, key(1), "a", "one", "/w/a.fugue");
        storeData(w, key(2), "b", "two", "/w/a.fugue");
        storeData(w, key(3), "a", "three", "/w/b.fugue");
        // key 2 is also used by workflow b.
        QVERIFY(w.cache->lookup(key(2), UsedBy{"/w/b.fugue", "b"}, *w.hashes).has_value());

        QCOMPARE(w.cache->clear("/w/a.fugue", "a"), QStringList{key(1)});
        QVERIFY(w.cache->contains(key(2)));

        // Clearing workflow a leaves what b still uses.
        QVERIFY(w.cache->clear("/w/a.fugue").isEmpty());
        QVERIFY(w.cache->contains(key(2)));
        QCOMPARE(w.cache->entries().size(), 2);

        QCOMPARE(w.cache->clear("/w/b.fugue").size(), 2);
        QVERIFY(w.cache->entries().isEmpty());
        QCOMPARE(w.cache->size(), qint64(0));
    }

    void clearAllKeepsFrozenResults() {
        World w = world();
        storeData(w, key(1), "a", "one");
        storeData(w, key(2), "b", "two");
        QVERIFY(w.cache->freeze(key(2)));
        QCOMPARE(w.cache->clear(), QStringList{key(1)});
        QVERIFY(w.cache->contains(key(2)));
        QCOMPARE(w.cache->size(), qint64(3));
    }

    // --- garbage collection (10.5) ---

    void garbageIsCollectedAfterTheGracePeriod() {
        World w = world();
        const QString kept = storeData(w, key(1), "a", "kept");
        const QString orphan = w.root + "/blobs/" + sha256(QByteArray("orphan")).mid(7) + ".bin";
        QFile file(orphan);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("orphan");
        file.close();
        // A record whose blob is gone.
        const QString gone = storeData(w, key(2), "b", "gone");
        QVERIFY(QFile::remove(gone));

        m_now = QDateTime::currentDateTimeUtc().addDays(6);
        w.cache->collectGarbage();
        QVERIFY(QFile::exists(orphan));
        QVERIFY(!w.cache->contains(key(2)));

        m_now = QDateTime::currentDateTimeUtc().addDays(8);
        w.cache->collectGarbage();
        QVERIFY(!QFile::exists(orphan));
        QVERIFY(QFile::exists(kept));
        QVERIFY(w.cache->contains(key(1)));
    }

    void endedRunFoldersGoAfterTheGracePeriod() {
        const QString runs = m_dir.filePath("runs");
        const auto old = RunFolder::create(runs, "old");
        const auto recent = RunFolder::create(runs, "recent");
        const auto running = RunFolder::create(runs, "running");
        QVERIFY(old.ok() && recent.ok() && running.ok());
        old.value().writeState(QJsonObject{{"status", "ok"}, {"ended", "2026-11-10T12:00:00.000Z"}});
        recent.value().writeState(QJsonObject{{"status", "failed"}, {"ended", "2026-11-18T12:00:00.000Z"}});
        running.value().writeState(QJsonObject{{"status", "running"}, {"started", "2026-11-01T12:00:00.000Z"}});

        QCOMPARE(removeEndedRunFolders(runs, m_now), QStringList{"old"});
        QVERIFY(!QDir(runs + "/old").exists());
        QVERIFY(QDir(runs + "/recent").exists());
        QVERIFY(QDir(runs + "/running").exists());
    }
};

QTEST_GUILESS_MAIN(TestResultCache)
#include "test_result_cache.moc"
