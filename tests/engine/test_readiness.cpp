// Getting ready with one approval (spec 6.6-6.8, 8.9, 11.10; #379): the plan lists the packs, their
// environments and the missing models with totals; the executor runs the list on one yes, with no
// second question, all or nothing per item, stopping at the first failure.
//
// Seams: buildReadinessPlan / runReadinessPlan over real folders, a loopback HTTP server for the models.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/pack_loader.h"
#include "engine/readiness.h"
#include "engine/registry.h"
#include "fake_http.h"

using namespace fugue;
using fugue::test::FakeHttpServer;
using fugue::test::HttpResponse;

namespace {

void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(bytes);
}

void writeJson(const QString &path, const QJsonObject &object) { write(path, QJsonDocument(object).toJson()); }

// A pack folder `root/<id>` with one command node and a setup.
QString makePack(const QString &root, const QString &id, const QJsonObject &setup) {
    const QString dir = root + "/" + id;
    QJsonObject pack{{"manifest", 1}, {"id", id}, {"version", "1.0.0"}, {"name", "Segment"}, {"nodes", QJsonArray{"tool"}}};
    pack["setup"] = setup;
    writeJson(dir + "/pack.json", pack);
    writeJson(dir + "/tool/node.json",
              QJsonObject{{"manifest", 1}, {"id", id + "/tool"}, {"version", "1.0.0"}, {"name", "Tool"},
                          {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                          {"needs", QJsonObject{{"network", true}}},
                          {"inputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}},
                          {"outputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}}});
    write(dir + "/tool/run.sh", "#!/bin/sh\n");
    return dir;
}

QJsonObject setupOf(const QString &script, qint64 size = -1) {
    QJsonObject setup{{"command", QJsonArray{"sh", "-c", script}}, {"description", "Makes the environment"}};
    if (size >= 0)
        setup["size"] = double(size);
    return setup;
}

} // namespace

class TestReadiness : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    Registry m_registry;
    std::unique_ptr<FileHashes> m_hashes;
    std::unique_ptr<TrustStore> m_trust;
    std::unique_ptr<FakeHttpServer> m_server;
    QByteArray m_bytes = QByteArray(5000, 'a');
    QByteArray m_other = QByteArray(3000, 'b');

    QString packs() const { return m_dir.filePath("packs"); }
    QString models() const { return m_dir.filePath("models"); }

    QJsonObject model(const QString &name, const QByteArray &bytes, const QString &path, qint64 size) const {
        return QJsonObject{{"resource", "io.test.pack/lora"}, {"hash", sha256(bytes)}, {"name", name},
                           {"source", QString::fromLatin1(m_server->origin() + path.toLatin1())}, {"size", double(size)}};
    }
    ReadinessEnv env() {
        ReadinessEnv e{ModelLibrary({models()}, &m_registry)};
        e.registry = &m_registry;
        e.hashes = m_hashes.get();
        e.trust = m_trust.get();
        e.packsDir = packs();
        e.freeSpace = [](const QString &) { return qint64(4) << 30; };
        return e;
    }
    ReadinessRequest request(const QString &packFolder, const QJsonArray &wanted) const {
        ReadinessRequest r;
        WantedPack pack;
        pack.id = "io.test.segment";
        pack.version = "1.0.0";
        pack.needsInstall = true;
        pack.hasSource = true;
        pack.source = PackSource{PackSource::Kind::Path, packFolder, {}, {}};
        r.packs << pack;
        r.models = wanted;
        return r;
    }

private slots:
    void initTestCase() {
        DeclaredPortType declared;
        declared.type = *PortType::parse("io.test.pack/lora");
        declared.kind = PortKind::Resource;
        declared.name = "A model";
        declared.extensions = {".safetensors"};
        declared.folders = {"loras"};
        QVERIFY(m_registry.add(declared).isEmpty());
    }
    void init() {
        QDir(m_dir.path()).removeRecursively();
        QDir().mkpath(m_dir.path());
        m_hashes = std::make_unique<FileHashes>(m_dir.filePath("hashes.json"));
        m_trust = std::make_unique<TrustStore>(m_dir.filePath("trust.json"));
        m_server = std::make_unique<FakeHttpServer>();
        QVERIFY(m_server->listen());
        m_server->on("GET", "/a.safetensors", [this](const auto &) { return HttpResponse{200, m_bytes, "application/octet-stream"}; });
        m_server->on("GET", "/b.safetensors", [this](const auto &) { return HttpResponse{200, m_other, "application/octet-stream"}; });
    }

    void thePlanListsPacksEnvironmentsAndModelsWithTotals() {
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("true", 27000000));
        const QJsonArray wanted{model("a.safetensors", m_bytes, "/a.safetensors", 1200000000), model("b.safetensors", m_other, "/b.safetensors", -1)};
        const ReadinessPlan plan = buildReadinessPlan(request(folder, wanted), env());
        QCOMPARE(plan.packs.size(), 1);
        QCOMPARE(plan.models.size(), 2);
        QCOMPARE(plan.environmentCount(), 1);
        QCOMPARE(plan.packs.at(0).pack.permissions, QStringList{"network"});
        QCOMPARE(plan.downloadBytes, qint64(1200000000));
        QCOMPARE(plan.unknownSizes, 1);
        QCOMPARE(plan.environmentBytes, qint64(27000000));
        QCOMPARE(plan.destination, models());
        QCOMPARE(plan.models.at(0).host, QString("127.0.0.1"));
        QVERIFY(plan.headline().contains("install 1 pack, set up 1 environment and download 2 models"));
        QVERIFY(!plan.lowSpace());
        ReadinessEnv small = env();
        small.freeSpace = [](const QString &) { return qint64(1000); };
        QVERIFY(buildReadinessPlan(request(folder, wanted), small).lowSpace());
        // Nothing was installed or downloaded by listing it.
        QVERIFY(!QFileInfo::exists(packs() + "/io.test.segment"));
        QVERIFY(m_server->requests().isEmpty());
    }

    void oneYesInstallsTrustsSetsUpAndDownloads() {
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("touch environment.ok"));
        const ReadinessPlan plan = buildReadinessPlan(
            request(folder, {model("a.safetensors", m_bytes, "/a.safetensors", m_bytes.size()), model("b.safetensors", m_other, "/b.safetensors", -1)}), env());
        QList<RowState> last(plan.rowCount(), RowState::Waiting);
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        run.onRow = [&](int row, RowState state, const QString &) { last[row] = state; };
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY2(outcome.ok, outcome.errors.isEmpty() ? "" : qPrintable(outcome.errors.first().message));
        QVERIFY(outcome.packsChanged);
        QCOMPARE(last, (QList<RowState>{RowState::Done, RowState::Done, RowState::Done}));
        QVERIFY(QFileInfo(packs() + "/io.test.segment/environment.ok").isFile()); // its setup ran
        QVERIFY(m_trust->isTrusted("io.test.segment", "1.0.0"));                 // and it is trusted, from the same yes
        QCOMPARE(QFileInfo(models() + "/loras/a.safetensors").size(), qint64(m_bytes.size()));
        QVERIFY(QFileInfo(models() + "/loras/b.safetensors").isFile());
    }

    void aFailedSetupLeavesNothingAndStartsNoDownload() {
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("exit 3"));
        const ReadinessPlan plan = buildReadinessPlan(request(folder, {model("a.safetensors", m_bytes, "/a.safetensors", -1)}), env());
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY(!outcome.ok);
        QCOMPARE(outcome.failedRow, 0);
        QCOMPARE(outcome.errors.first().code, QString("setup_failed"));
        QCOMPARE(QDir(packs()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList());
        QVERIFY(!m_trust->isTrusted("io.test.segment", "1.0.0"));
        QVERIFY(m_server->requests().isEmpty());
        QCOMPARE(outcome.retry.size(), 2); // the pack and the model are what a second run does
    }

    void aFailedModelStopsTheRestAndKeepsWhatWasDone() {
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("true"));
        m_server->on("GET", "/a.safetensors", [](const auto &) { return HttpResponse{404, "no", "text/plain"}; });
        const ReadinessPlan plan = buildReadinessPlan(
            request(folder, {model("a.safetensors", m_bytes, "/a.safetensors", -1), model("b.safetensors", m_other, "/b.safetensors", -1)}), env());
        QList<RowState> last(plan.rowCount(), RowState::Waiting);
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        run.onRow = [&](int row, RowState state, const QString &) { last[row] = state; };
        run.retries = 0;
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY(!outcome.ok);
        QCOMPARE(outcome.failedRow, 1);
        QCOMPARE(last, (QList<RowState>{RowState::Done, RowState::Failed, RowState::Skipped}));
        QVERIFY(m_trust->isTrusted("io.test.segment", "1.0.0")); // the pack is whole and stays
        QVERIFY(!QFileInfo::exists(models() + "/loras/a.safetensors"));
        QVERIFY(!QFileInfo::exists(models() + "/loras/b.safetensors"));
        QVERIFY(m_server->requestsTo("GET", "/b.safetensors").isEmpty());
        QCOMPARE(outcome.retry, (QStringList{"download a.safetensors", "download b.safetensors"}));
    }

    void aPackThatChangedSinceItWasShownIsRefused() {
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("true"));
        const ReadinessPlan plan = buildReadinessPlan(request(folder, {}), env());
        makePack(m_dir.filePath("source"), "io.test.segment", setupOf("touch evil")); // another setup command
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY(!outcome.ok);
        QCOMPARE(outcome.errors.first().code, QString("pack_changed"));
        QVERIFY(!QFileInfo::exists(packs() + "/io.test.segment"));
    }

    void anInstalledPackThatIsNotTrustedIsTrustedByTheSameYes() {
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("true"));
        PackPrompts yes;
        yes.install = yes.trust = yes.runSetup = [](const PackSummary &) { return Answer::Yes; };
        QVERIFY(addPack(folder, {}, packs(), *m_trust, yes));
        Registry loaded;
        loadPacks(loaded, {packs()});
        ReadinessEnv e = env();
        e.registry = &loaded;
        ReadinessRequest r = request(folder, {});
        r.packs.first().needsInstall = false;
        const ReadinessPlan plan = buildReadinessPlan(r, e);
        QCOMPARE(plan.packs.size(), 1);
        QCOMPARE(plan.trustOnlyCount(), 1);
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        QVERIFY(runReadinessPlan(plan, run).ok);
        QVERIFY(m_trust->isTrusted("io.test.segment", "1.0.0"));
    }
};

QTEST_GUILESS_MAIN(TestReadiness)
#include "test_readiness.moc"
