// Live node types (#70, spec 6.5): the registry filled again from disk when a pack folder
// changes, from the window or from outside it (`fugue pack add`, another window's library save).
//
// Seam: RegistryReloader over a real folder of packs, watched with QFileSystemWatcher.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/pack_loader.h"
#include "engine/registry.h"
#include "engine/registry_reloader.h"

using namespace fugue;

namespace {

void writeJson(const QString &path, const QJsonObject &object) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(QJsonDocument(object).toJson());
}

QJsonObject nodeJson(const QString &id, const QString &name) {
    return {{"manifest", 1},
            {"id", id},
            {"version", "1.0.0"},
            {"name", name},
            {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
            {"inputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}},
            {"outputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}}};
}

// The pack io.example.tools in `root` with one node, `tool`, named `name`.
void writePack(const QString &root, const QString &name) {
    const QString dir = root + "/io.example.tools";
    writeJson(dir + "/pack.json", {{"manifest", 1}, {"id", "io.example.tools"}, {"version", "1.0.0"},
                                   {"name", "Tools"}, {"nodes", QJsonArray{"tool"}}});
    writeJson(dir + "/tool/node.json", nodeJson("io.example.tools/tool", name));
}

} // namespace

class TestRegistryReloader : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString packs() const { return m_dir.filePath("packs"); }

    std::unique_ptr<RegistryReloader> reloaderFor(Registry &registry) {
        const QString root = packs();
        auto fill = [root](Registry &into) { loadPacks(into, {root}); };
        fill(registry);
        auto reloader = std::make_unique<RegistryReloader>(&registry, fill, [root] { return QStringList{root}; });
        reloader->setSettleMs(50);
        return reloader;
    }

private slots:
    void init() { QDir(packs()).removeRecursively(); }

    void aPackCopiedInEditedAndRemovedShowsWithoutARestart() {
        Registry registry;
        auto reloader = reloaderFor(registry); // the packs folder does not exist yet
        QSignalSpy reloaded(reloader.get(), &RegistryReloader::reloaded);
        QVERIFY(!registry.nodeType("io.example.tools/tool"));

        writePack(packs(), "Tool");
        QTRY_VERIFY_WITH_TIMEOUT(registry.nodeType("io.example.tools/tool"), 5000);
        QCOMPARE(reloaded.count(), 1);

        // An edit in place of a node.json the window has loaded.
        writeJson(packs() + "/io.example.tools/tool/node.json", nodeJson("io.example.tools/tool", "Sharper tool"));
        QTRY_COMPARE_WITH_TIMEOUT(registry.nodeType("io.example.tools/tool")->name, QStringLiteral("Sharper tool"), 5000);

        QVERIFY(QDir(packs() + "/io.example.tools").removeRecursively());
        QTRY_VERIFY_WITH_TIMEOUT(!registry.nodeType("io.example.tools/tool"), 5000);
    }

    void aChangeThatLeavesTheTypesAsTheyWereIsNoReload() {
        writePack(packs(), "Tool");
        Registry registry;
        auto reloader = reloaderFor(registry);
        QSignalSpy reloaded(reloader.get(), &RegistryReloader::reloaded);
        QFile note(packs() + "/io.example.tools/README.md");
        QVERIFY(note.open(QIODevice::WriteOnly));
        note.write("notes");
        note.close();
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
        writePack(packs(), "Tool");
        for (int i = 0; i < 5; ++i) {
            writeJson(packs() + "/io.example.tools/tool/node.json", nodeJson("io.example.tools/tool", QString("Tool %1").arg(i)));
            QTest::qWait(30);
        }
        QTRY_COMPARE_WITH_TIMEOUT(reloaded.count(), 1, 5000);
        QTest::qWait(500);
        QCOMPARE(reloaded.count(), 1);
        QCOMPARE(registry.nodeType("io.example.tools/tool")->name, QStringLiteral("Tool 4"));
    }
};

QTEST_GUILESS_MAIN(TestRegistryReloader)
#include "test_registry_reloader.moc"
