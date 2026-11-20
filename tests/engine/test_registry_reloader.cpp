// Live node types (#70, spec 6.5): the registry filled again from disk when a pack folder
// changes, from the window or from outside it (`fugue pack add`, another window's library save),
// and a reload when trust is given from outside (`fugue pack trust`).
//
// Seam: RegistryReloader over a real folder of packs, watched with QFileSystemWatcher.

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "../disk_pack.h"
#include "engine/pack_loader.h"
#include "engine/registry.h"
#include "engine/registry_reloader.h"
#include "engine/trust_store.h"

using namespace fugue;
using pack_fixture::writeFile;
using pack_fixture::writeSayPack;

namespace {

const char kSay[] = "io.example.tools/say";

} // namespace

class TestRegistryReloader : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString packs() const { return m_dir.filePath("packs"); }
    QStringList m_folders;

    std::unique_ptr<RegistryReloader> reloaderFor(Registry &registry) {
        m_folders = {packs()};
        auto fill = [this](Registry &into) { loadPacks(into, m_folders); };
        fill(registry);
        auto reloader = std::make_unique<RegistryReloader>(&registry, fill, [this] { return m_folders; });
        reloader->setSettleMs(50);
        return reloader;
    }

private slots:
    void init() {
        QDir(packs()).removeRecursively();
        QDir(m_dir.filePath("other")).removeRecursively();
        QFile::remove(m_dir.filePath("state/trust.json"));
    }

    void aPackCopiedInUpdatedAndRemovedShowsWithoutARestart() {
        Registry registry;
        auto reloader = reloaderFor(registry); // the packs folder does not exist yet
        QSignalSpy reloaded(reloader.get(), &RegistryReloader::reloaded);
        QVERIFY(!registry.nodeType(kSay));

        QVERIFY(writeSayPack(packs(), "first"));
        QTRY_VERIFY_WITH_TIMEOUT(registry.nodeType(kSay), 5000);
        QCOMPARE(reloaded.count(), 1);
        const QString seal = registry.nodeType(kSay)->seal;

        // Its files written again in place (an update): a new seal, a new version.
        QVERIFY(writeSayPack(packs(), "second", "1.1.0"));
        QTRY_COMPARE_WITH_TIMEOUT(registry.nodeType(kSay)->version.toString(), QStringLiteral("1.1.0"), 5000);
        QVERIFY(registry.nodeType(kSay)->seal != seal);

        QVERIFY(QDir(packs() + "/io.example.tools").removeRecursively());
        QTRY_VERIFY_WITH_TIMEOUT(!registry.nodeType(kSay), 5000);
    }

    void aChangeThatLeavesTheTypesAsTheyWereIsNoReload() {
        QVERIFY(writeSayPack(packs(), "first"));
        Registry registry;
        auto reloader = reloaderFor(registry);
        QSignalSpy reloaded(reloader.get(), &RegistryReloader::reloaded);
        QVERIFY(writeFile(packs() + "/io.example.tools/README.md", "notes"));
        QTest::qWait(400);
        QCOMPARE(reloaded.count(), 0);

        // Asked for (an install, a trust), it always says so.
        reloader->reload();
        QCOMPARE(reloaded.count(), 1);
    }

    void manyEventsInARowAreOneReload() {
        Registry registry;
        auto reloader = reloaderFor(registry);
        reloader->setSettleMs(300);
        QSignalSpy reloaded(reloader.get(), &RegistryReloader::reloaded);
        for (int i = 0; i < 5; ++i) {
            QVERIFY(writeSayPack(packs(), "again", QStringLiteral("1.0.%1").arg(i)));
            QTest::qWait(30);
        }
        QTRY_COMPARE_WITH_TIMEOUT(reloaded.count(), 1, 5000);
        QTest::qWait(500);
        QCOMPARE(reloaded.count(), 1);
        QCOMPARE(registry.nodeType(kSay)->version.toString(), QStringLiteral("1.0.4"));
    }

    // pack_paths changed in the settings: the next fill reads the new folders and watches them.
    void aNewPackPathIsReadAndWatched() {
        Registry registry;
        auto reloader = reloaderFor(registry);
        const QString other = m_dir.filePath("other");
        QVERIFY(QDir().mkpath(other));
        m_folders << other;
        reloader->reloadSoon();
        QTest::qWait(200);
        QVERIFY(writeSayPack(other, "elsewhere"));
        QTRY_VERIFY_WITH_TIMEOUT(registry.nodeType(kSay), 5000);
    }

    // `fugue pack trust` writes trust.json from outside the window: a reload, though no type
    // changed, so the window asks about trust again. Other files of its folder are no reload.
    void trustGivenFromOutsideIsAReload() {
        Registry registry;
        auto reloader = reloaderFor(registry);
        const QString trustFile = m_dir.filePath("state/trust.json");
        QVERIFY(QDir().mkpath(m_dir.filePath("state")));
        reloader->setTrustFile(trustFile);
        QSignalSpy reloaded(reloader.get(), &RegistryReloader::reloaded);

        QVERIFY(writeFile(m_dir.filePath("state/hashes.json"), "{}"));
        QTest::qWait(400);
        QCOMPARE(reloaded.count(), 0);

        QVERIFY(TrustStore(trustFile).trust("io.example.tools", "1.0.0").isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(reloaded.count(), 1, 5000);
        QVERIFY(TrustStore(trustFile).trust("io.example.tools", "1.1.0").isEmpty()); // replaced by a rename
        QTRY_COMPARE_WITH_TIMEOUT(reloaded.count(), 2, 5000);
    }
};

QTEST_GUILESS_MAIN(TestRegistryReloader)
#include "test_registry_reloader.moc"
