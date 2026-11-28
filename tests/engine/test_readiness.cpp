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
#include <QElapsedTimer>
#include <QProcess>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <future>

#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/pack_loader.h"
#include "engine/readiness.h"
#include "engine/sandbox.h"
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
QString makePack(const QString &root, const QString &id, const QJsonObject &setup, const QString &modelType = {}) {
    const QString dir = root + "/" + id;
    QJsonObject pack{{"manifest", 1}, {"id", id}, {"version", "1.0.0"}, {"name", "Segment"}, {"nodes", QJsonArray{"tool"}}};
    if (!modelType.isEmpty())
        pack["types"] = QJsonArray{QJsonObject{{"id", id + "/" + modelType}, {"version", 1}, {"kind", "resource"}, {"name", "A model"},
                                               {"extensions", QJsonArray{".safetensors"}}, {"folders", QJsonArray{modelType + "s"}}}};
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
        if (!sandbox::available()) // these tests run real setup commands; the sandbox itself is tested in test_sandbox
            sandbox::runSetupUnsandboxedForTest(true);
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

    // #594: a model only another branch of a Switch holds is a non-blocking row apart: not in the models, the totals, the headline
    // or empty(); a missing one with no source is no problem; chosen by the person it is a row like the others, marked optional.
    void anOptionalModelIsListedApartAndCostsNothingUntilChosen() {
        const QJsonArray wanted{model("a.safetensors", m_bytes, "/a.safetensors", 1200)};
        QJsonObject noSource = model("b.safetensors", m_other, "/b.safetensors", 900);
        noSource.remove("source");
        ReadinessRequest r;
        r.models = wanted;
        r.optionalModels = QJsonArray{model("c.safetensors", QByteArray(10, 'c'), "/c.safetensors", 700), noSource};
        const ReadinessPlan plan = buildReadinessPlan(r, env());
        QCOMPARE(plan.models.size(), 1);
        QCOMPARE(plan.optionalModels.size(), 1); // the one with no source is left out
        QCOMPARE(plan.optionalModels.at(0).name, QString("c.safetensors"));
        QVERIFY(plan.problems.isEmpty());
        QCOMPARE(plan.downloadBytes, qint64(1200));
        QVERIFY(plan.optionalModels.at(0).size == 700 && !plan.optionalModels.at(0).target.path.isEmpty());
        QVERIFY(plan.toJson().contains("optional_models"));
        ReadinessRequest only;
        only.optionalModels = r.optionalModels;
        QVERIFY(buildReadinessPlan(only, env()).empty()); // nothing blocks, so nothing to do
        // Chosen: a row of the plan, counted, marked optional.
        r.optionalChosen.insert(sha256(QByteArray(10, 'c')));
        const ReadinessPlan chosen = buildReadinessPlan(r, env());
        QCOMPARE(chosen.models.size(), 2);
        QVERIFY(chosen.optionalModels.isEmpty());
        QVERIFY(!chosen.models.at(0).optional && chosen.models.at(1).optional);
        QCOMPARE(chosen.downloadBytes, qint64(1900));
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
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("touch .venv/environment.ok"));
        const ReadinessPlan plan = buildReadinessPlan(
            request(folder, {model("a.safetensors", m_bytes, "/a.safetensors", m_bytes.size()), model("b.safetensors", m_other, "/b.safetensors", -1)}), env());
        QList<RowState> last(plan.rowCount(), RowState::Waiting);
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        run.onRow = [&](int row, RowState state, const QString &) { last[row] = state; };
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY2(outcome.ok, outcome.errors.isEmpty() ? "" : qPrintable(outcome.errors.first().message));
        QVERIFY(outcome.packsChanged);
        QCOMPARE(last, (QList<RowState>{RowState::Done, RowState::Done, RowState::Done}));
        QVERIFY(QFileInfo(packs() + "/io.test.segment/.venv/environment.ok").isFile()); // its setup ran
        QVERIFY(m_trust->isTrusted("io.test.segment", "1.0.0"));                 // and it is trusted, from the same yes
        QCOMPARE(QFileInfo(models() + "/loras/a.safetensors").size(), qint64(m_bytes.size()));
        QVERIFY(QFileInfo(models() + "/loras/b.safetensors").isFile());
    }

    // #418: a model whose type the pack of the same plan declares is a row of the list, and the one yes covers it.
    void aModelOfAPacksTypeIsDownloadedInTheSameRun() {
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("true"), "onnx");
        QJsonObject wanted = model("a.safetensors", m_bytes, "/a.safetensors", m_bytes.size());
        wanted["resource"] = "io.test.segment/onnx";
        const ReadinessPlan plan = buildReadinessPlan(request(folder, {wanted}), env());
        QVERIFY(plan.problems.isEmpty());
        QCOMPARE(plan.models.size(), 1);
        QVERIFY(plan.models.at(0).deferred);
        QCOMPARE(plan.models.at(0).typeFrom, QString("Segment"));
        QCOMPARE(plan.downloadBytes, qint64(m_bytes.size())); // in the totals
        QVERIFY(plan.lines().join("\n").contains("type from Segment"));
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY2(outcome.ok, outcome.errors.isEmpty() ? "" : qPrintable(outcome.errors.first().message));
        QVERIFY(outcome.errors.isEmpty());
        QCOMPARE(QFileInfo(models() + "/onnxs/a.safetensors").size(), qint64(m_bytes.size()));
    }

    void aTypeThePackDoesNotDeclareAfterInstallIsAClearError() {
        // A git pack is not read before the yes, so its type is only found missing after the install.
        const QString source = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("true"), "other");
        QJsonObject wanted = model("a.safetensors", m_bytes, "/a.safetensors", -1);
        wanted["resource"] = "io.test.segment/onnx";
        ReadinessRequest r = request(m_dir.filePath("x"), {wanted});
        r.packs.first().source = PackSource{PackSource::Kind::Git, "https://example.invalid/segment.git", "v1", {}};
        const ReadinessPlan plan = buildReadinessPlan(r, env());
        QVERIFY(plan.models.at(0).deferred);
        int calls = 0;
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        run.fetch = fetcher(source, &calls);
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY(!outcome.ok);
        QCOMPARE(outcome.failedRow, 1);
        QCOMPARE(outcome.errors.first().code, QString("pack_invalid"));
        QVERIFY(outcome.errors.first().message.contains("does not declare the model type io.test.segment/onnx"));
        QVERIFY(m_server->requests().isEmpty());
    }

    void aFailedSetupLeavesNothingAndStartsNoDownload() {
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("exit 3"));
        const ReadinessPlan plan = buildReadinessPlan(request(folder, {model("a.safetensors", m_bytes, "/a.safetensors", -1)}), env());
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY(!outcome.ok);
        QCOMPARE(outcome.failedRow, 0);
        QCOMPARE(outcome.errors.first().code, QString("setup_failed"));
        QCOMPARE(QDir(packs()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList()); // only the plan's own staging copy is hidden here
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

    // Review of #379: free space is checked before each download, not only when the list was shown.
    void aDownloadThatNoLongerFitsIsNotStarted() {
        const ReadinessPlan plan = buildReadinessPlan(request(makePack(m_dir.filePath("source"), "io.test.segment", setupOf("true")),
                                                              {model("a.safetensors", m_bytes, "/a.safetensors", m_bytes.size())}), env());
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        run.freeSpace = [](const QString &) { return qint64(10); }; // the disk filled since the list was shown
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY(!outcome.ok);
        QCOMPARE(outcome.failedRow, 1);
        QVERIFY(outcome.errors.first().message.contains("not enough free space"));
        QVERIFY(m_server->requests().isEmpty());
    }

    // What is installed is the copy the list showed: the source changing afterwards changes nothing.
    void theInstalledPackIsTheCopyTheListShowed() {
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("touch .venv/shown"));
        const ReadinessPlan plan = buildReadinessPlan(request(folder, {}), env());
        makePack(m_dir.filePath("source"), "io.test.segment", setupOf("touch .venv/evil")); // another setup command, after the list
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY(outcome.ok);
        QVERIFY(QFileInfo::exists(packs() + "/io.test.segment/.venv/shown"));
        QVERIFY(!QFileInfo::exists(packs() + "/io.test.segment/.venv/evil"));
    }

    // Review of #379: nothing is fetched before the yes. A git pack's row shows what the workflow records.
    ReadinessRequest gitRequest(const QString &location) const {
        ReadinessRequest r = request(m_dir.filePath("x"), {});
        r.packs.first().source = PackSource{PackSource::Kind::Git, location, "v1", {}};
        return r;
    }
    // A fetcher that copies `folder` in, counting its calls: nothing reaches a network.
    std::function<Result<QString>(const PackSource &, const QString &)> fetcher(const QString &folder, int *calls) const {
        return [folder, calls](const PackSource &, const QString &work) -> Result<QString> {
            ++*calls;
            QProcess::execute("cp", {"-r", folder, work});
            return QString("abc123");
        };
    }

    void aGitPackIsNotFetchedBeforeTheYes() {
        int calls = 0;
        ReadinessEnv e = env();
        e.prompts.fetch = fetcher(makePack(m_dir.filePath("source"), "io.test.segment", setupOf("true")), &calls);
        const ReadinessPlan plan = buildReadinessPlan(gitRequest("https://example.com/p.git"), e);
        QCOMPARE(plan.packs.size(), 1);
        QVERIFY(plan.packs.first().pending);
        QCOMPARE(calls, 0);
        QVERIFY(plan.problems.isEmpty());
        QCOMPARE(QDir(packs()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList()); // no staging either
        QVERIFY(plan.lines().join("\n").contains("Details after download"));
    }

    void afterTheYesOnlyHttpsIsFetchedAndMorePermissionsAskOnce() {
        int calls = 0;
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("true")); // declares network
        // A source that is not https is refused without a fetch.
        ReadinessPlan plan = buildReadinessPlan(gitRequest("file:///somewhere/p.git"), env());
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        run.fetch = fetcher(folder, &calls);
        ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY(!outcome.ok);
        QCOMPARE(calls, 0);
        QVERIFY(!QFileInfo::exists(packs() + "/io.test.segment"));

        // https: fetched after the yes; it declares network, which was not shown, so it asks; No skips that row alone.
        plan = buildReadinessPlan(gitRequest("https://example.com/p.git"), env());
        QStringList asked;
        run.allowMore = [&](const PackSummary &, const QStringList &permissions) { asked = permissions; return false; };
        outcome = runReadinessPlan(plan, run);
        QCOMPARE(calls, 1);
        QCOMPARE(asked, QStringList{"network"});
        QVERIFY(!outcome.ok);
        QVERIFY(!QFileInfo::exists(packs() + "/io.test.segment"));
        QCOMPARE(QDir(packs()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList()); // staging gone

        // Allowed: installed from that clone, trusted, no further question.
        int questions = 0;
        run.allowMore = [&](const PackSummary &, const QStringList &) { ++questions; return true; };
        outcome = runReadinessPlan(plan, run);
        QVERIFY(outcome.ok);
        QCOMPARE(questions, 1);
        QVERIFY(m_trust->isTrusted("io.test.segment", "1.0.0"));
    }

    void aPackThatIsNotTheOneNamedIsRefused() {
        int calls = 0;
        const QString other = makePack(m_dir.filePath("source"), "io.test.other", setupOf("true"));
        // Fetched after the yes: another id than the workflow names.
        const ReadinessPlan git = buildReadinessPlan(gitRequest("https://example.com/p.git"), env());
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        run.fetch = fetcher(other, &calls);
        run.allowMore = [](const PackSummary &, const QStringList &) { return true; };
        const ReadinessOutcome outcome = runReadinessPlan(git, run);
        QVERIFY(!outcome.ok);
        QCOMPARE(outcome.errors.first().code, QString("pack_invalid"));
        QVERIFY(!QFileInfo::exists(packs() + "/io.test.other"));
        // A folder: refused when the plan is built.
        const ReadinessPlan folder = buildReadinessPlan(request(other, {}), env());
        QVERIFY(folder.packs.isEmpty());
        QCOMPARE(folder.problems.size(), 1);
    }

    void aPackWithASymbolicLinkIsRefused() {
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("true"));
        QVERIFY(QFile::link("/etc/passwd", folder + "/tool/leak"));
        const ReadinessPlan plan = buildReadinessPlan(request(folder, {}), env());
        QVERIFY(plan.packs.isEmpty());
        QVERIFY(!plan.problems.isEmpty());
        QVERIFY(plan.problems.first().message.contains("symbolic link") || plan.problems.first().hint.contains("symbolic"));
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

    // #521: a pack whose setup never ran to its end here (no marker; one installed before the marker existed is the same) is
    // offered the setup by Get ready, trusted or not, and the one yes runs it.
    void anInstalledPackWithASetupAndNoMarkerIsOfferedTheSetup() {
        const QString folder = makePack(m_dir.filePath("source"), "io.test.segment", setupOf("touch .venv/environment.ok"));
        PackPrompts yes;
        yes.install = yes.trust = yes.runSetup = [](const PackSummary &) { return Answer::Yes; };
        QVERIFY(addPack(folder, {}, packs(), *m_trust, yes).ok());
        QVERIFY(trustPack("io.test.segment", packs(), *m_trust, yes).ok());
        const QString installed = packs() + "/io.test.segment";
        QVERIFY(QFileInfo(installed + "/.venv/environment.ok").isFile());
        Registry loaded;
        loadPacks(loaded, {packs()});
        ReadinessEnv e = env();
        e.registry = &loaded;
        ReadinessRequest r = request(folder, {});
        r.packs.first().needsInstall = false;
        QVERIFY(buildReadinessPlan(r, e).packs.isEmpty()); // set up, trusted: nothing to do

        // The environment is gone and so is the marker (what a pack installed before markers looks like).
        QVERIFY(QFile::remove(installed + "/.venv/environment.ok"));
        QVERIFY(QFile::remove(installed + "/.fugue-setup.json"));
        const ReadinessPlan plan = buildReadinessPlan(r, e);
        QCOMPARE(plan.packs.size(), 1);
        QCOMPARE(plan.packs.first().action, ReadinessPack::Action::Setup);
        QCOMPARE(plan.trustOnlyCount(), 0);
        QCOMPARE(plan.environmentCount(), 1);
        QVERIFY(plan.headline().contains("set up 1 environment"));
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        run.cancelled = nullptr;
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY(outcome.ok);
        QVERIFY(QFileInfo(installed + "/.venv/environment.ok").isFile());
        QVERIFY(buildReadinessPlan(r, e).packs.isEmpty()); // the marker is back

        // A setup that fails leaves no marker, so it is offered again.
        QVERIFY(QFile::remove(installed + "/.fugue-setup.json"));
        QVERIFY(QFile::remove(installed + "/.venv/environment.ok"));
        QCOMPARE(buildReadinessPlan(r, e).packs.size(), 1);
    }

    // Spec 8.9: a file with the handle's name and no remembered hash is taken to be the one, as `fugue validate` and the canvas
    // check take it, also in an extra model folder in ComfyUI's layout. The plan reads no model file to tell.
    void aFileWithTheNameAndNoRememberedHashIsNotDownloaded() {
        const QString comfy = m_dir.filePath("comfy");
        write(comfy + "/loras/a.safetensors", "not the bytes the hash names"); // only a run reads it and finds out
        ReadinessEnv e = env();
        e.library = ModelLibrary({models(), comfy}, &m_registry);
        const ReadinessPlan plan = buildReadinessPlan(request(makePack(m_dir.filePath("source"), "io.test.segment", setupOf("true")),
                                                              {model("a.safetensors", m_bytes, "/a.safetensors", m_bytes.size())}), e);
        QCOMPARE(plan.models.size(), 0);
        QVERIFY(plan.problems.isEmpty());
        QVERIFY(m_hashes->known(comfy + "/loras/a.safetensors").isEmpty()); // not hashed
    }

    // The freeze of #379's sheet: a download reported every chunk the network handed over (thousands a second), and each one
    // rebuilt the window's sheet. The observer now hears it at most every 100 ms, and once at the end.
    void aDownloadsProgressIsToldAFewTimesASecondNotPerChunk() {
        const QByteArray body(64 * 16384, 'z');
        std::promise<quint16> port;
        // A server on a thread of its own that sends the body in small, spaced chunks, as a real network does.
        QThread *server = QThread::create([&body, &port] {
            QTcpServer listener;
            port.set_value(listener.listen(QHostAddress::LocalHost, 0) ? listener.serverPort() : 0);
            if (!listener.waitForNewConnection(10000))
                return;
            QTcpSocket *socket = listener.nextPendingConnection();
            socket->waitForReadyRead(5000);
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nConnection: close\r\nContent-Length: "
                          + QByteArray::number(body.size()) + "\r\n\r\n");
            for (qsizetype at = 0; at < body.size(); at += 16384) {
                socket->write(body.mid(at, 16384));
                socket->waitForBytesWritten(5000);
                QThread::msleep(4);
            }
            socket->disconnectFromHost();
            if (socket->state() != QAbstractSocket::UnconnectedState)
                socket->waitForDisconnected(5000);
        });
        server->start();
        const auto stop = qScopeGuard([server] { server->wait(); delete server; });
        const quint16 at = port.get_future().get();
        QVERIFY(at != 0);
        QJsonObject wanted = model("a.safetensors", body, "/a.safetensors", body.size());
        wanted["source"] = QStringLiteral("http://127.0.0.1:%1/a.safetensors").arg(at);
        ReadinessRequest r;
        r.models = {wanted};
        const ReadinessPlan plan = buildReadinessPlan(r, env());
        QCOMPARE(plan.models.size(), 1);
        int told = 0;
        qint64 last = 0;
        ReadinessRun run{packs(), m_trust.get(), m_hashes.get()};
        run.onProgress = [&](int, qint64 got, qint64) { ++told; last = got; };
        QElapsedTimer clock;
        clock.start();
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY2(outcome.ok, outcome.errors.isEmpty() ? "" : qPrintable(outcome.errors.first().message));
        QCOMPARE(last, qint64(body.size())); // the end is always told
        // One every 100 ms of the run at most, plus the first and the last: never one per chunk (64 of them here).
        QVERIFY2(told <= clock.elapsed() / 100 + 2, qPrintable(QStringLiteral("%1 in %2 ms").arg(told).arg(clock.elapsed())));
    }
};

QTEST_GUILESS_MAIN(TestReadiness)
#include "test_readiness.moc"
