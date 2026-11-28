// The models of a paired helper (#735, spec 8.7): the list the host answers to `POST /fugue/v1/models`, kept per host, refreshed
// without blocking, and read by the pickers and readiness.
//
// Seam: HostModelCache against a fake host (tests/engine/fake_http.h) that speaks the contract; no helper, no network.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/host_models.h"
#include "engine/model_library.h"
#include "engine/model_offers.h"
#include "engine/port_type.h"
#include "engine/registry.h"
#include "fake_http.h"
#include "provider_support.h"

using namespace fugue;
using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

const QByteArray kKey = "tailnet-key-0123";
const QString kHash = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

QJsonObject entry(const QString &name, qint64 size, const QString &sha, const QString &family = {}) {
    return QJsonObject{{"name", name}, {"size", double(size)}, {"sha256", sha.isEmpty() ? QJsonValue() : QJsonValue(sha)},
                       {"folder", name.section('/', 0, 0)}, {"family", family.isEmpty() ? QJsonValue() : QJsonValue(family)}};
}

QJsonObject reply(const QJsonArray &models, int hashing = 0) {
    return QJsonObject{{"models", models}, {"truncated", false}, {"unhashed", 0}, {"hashing", hashing}, {"hash_started", 0}};
}

} // namespace

class TestHostModels : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    FakeHttpServer m_host;

    ProviderInstance instance(const QString &name = QStringLiteral("gx10")) const {
        ProviderInstance made;
        made.name = name;
        made.type = QStringLiteral("fugue-helper");
        made.address = QStringLiteral("http://gx10.example:8787"); // a paired host's (not this machine's) address, as isPairedHelper tells
        made.key = QString::fromLatin1(kKey);
        return made;
    }

    // How a refresh reaches the host: the same instance, at the fake.
    HostModelCache::Make reach() const {
        return [this](const ProviderInstance &paired) {
            ProviderInstance atTheFake = paired;
            atTheFake.address = QString::fromLatin1(m_host.origin());
            return createProvider(atTheFake);
        };
    }

    HostModelCache cache() const { return HostModelCache(m_dir.filePath(QStringLiteral("kept"))); }

    void answer(const QJsonObject &body, int status = 200) {
        m_host.on("POST", "/fugue/v1/models", [body, status](const HttpRequest &request) {
            if (request.headers.value("authorization") != "Bearer " + kKey)
                return HttpResponse{401, json({{"error", QJsonObject{{"code", "unauthorized"}, {"message", "no"}}}})};
            return HttpResponse{status, json(body)};
        });
    }

    HostModelList refreshed(HostModelCache &kept, bool hash = false, const QStringList &names = {}) {
        HostModelList got;
        bool done = false;
        kept.refresh(instance(), hash, names, [&](const HostModelList &list) { got = list; done = true; }, reach());
        const bool answered = QTest::qWaitFor([&] { return done; }, 5000);
        Q_UNUSED(answered); // an unanswered refresh shows as an empty list in the caller's own checks
        return got;
    }

private slots:
    void initTestCase() { QVERIFY(m_host.listen()); }

    void aRefreshKeepsTheHostsListAndReadsBackFromTheFile() {
        answer(reply(QJsonArray{entry("diffusion_models/z.gguf", 4000, kHash, "Z-Image"), entry("vae/ae.safetensors", 300, "")}));
        HostModelCache kept = cache();
        QVERIFY(!kept.cached(instance()).known()); // nothing yet
        const HostModelList list = refreshed(kept);
        QVERIFY(list.known());
        QVERIFY(list.error.isEmpty());
        QCOMPARE(list.models.size(), 2);
        // A second cache over the same folder (the next process) has it.
        HostModelCache again = cache();
        const HostModelList read = again.cached(instance());
        QCOMPARE(read.models.size(), 2);
        QCOMPARE(read.models.at(0).handleName(), QStringLiteral("z.gguf")); // relative to the type folder
        QCOMPARE(read.models.at(0).handleHash(), "sha256:" + kHash);
        QCOMPARE(read.models.at(0).family, QStringLiteral("Z-Image"));
        QVERIFY(read.models.at(1).sha256.isEmpty()); // not hashed yet: unknown, not wrong
        QVERIFY(read.byHash("sha256:" + kHash));
        QVERIFY(!read.byHash("sha256:" + QString(64, 'b')));
        QVERIFY(!read.byHash(QStringLiteral("not a hash")));
    }

    void aFailureKeepsTheLastListAndWhenItWasFetched() {
        answer(reply(QJsonArray{entry("vae/ae.safetensors", 300, kHash)}));
        HostModelCache kept = cache();
        const QDateTime first = refreshed(kept).fetchedAt;
        QVERIFY(first.isValid());
        m_host.on("POST", "/fugue/v1/models", [](const HttpRequest &) {
            return HttpResponse{500, json({{"error", QJsonObject{{"code", "helper_failed"}, {"message", "boom"}}}})};
        });
        const HostModelList failed = refreshed(kept);
        QCOMPARE(failed.models.size(), 1); // the last good list
        QCOMPARE(failed.fetchedAt.toSecsSinceEpoch(), first.toSecsSinceEpoch());
        QVERIFY(failed.error.contains("boom"));
        QVERIFY(kept.cached(instance()).error.contains("boom")); // and the file says so for the picker
    }

    void anOldHelperWithoutTheOperationIsUnknownNotEmpty() {
        m_host.on("POST", "/fugue/v1/models", [](const HttpRequest &) {
            return HttpResponse{404, json({{"error", QJsonObject{{"code", "not_found"}, {"message", "HTTP 404"}}}})};
        });
        HostModelCache kept = cache();
        const HostModelList list = refreshed(kept);
        QVERIFY(!list.supported);
        QVERIFY(list.models.isEmpty());
        QVERIFY(list.known());
        QVERIFY(!kept.hostsHavingAll({instance()}, {"sha256:" + kHash}).contains("gx10"));
    }

    void hashingIsAskedForOnlyOnRequestAndNamesTheFiles() {
        answer(reply(QJsonArray{entry("diffusion_models/z.gguf", 4000, "")}, 1));
        HostModelCache kept = cache();
        refreshed(kept);
        QJsonObject body = QJsonDocument::fromJson(m_host.requestsTo("POST", "/fugue/v1/models").last().body).object();
        QVERIFY(!body.contains("hash"));
        const HostModelList list = refreshed(kept, true, {QStringLiteral("diffusion_models/z.gguf")});
        body = QJsonDocument::fromJson(m_host.requestsTo("POST", "/fugue/v1/models").last().body).object();
        QCOMPARE(body.value("hash").toBool(), true);
        QCOMPARE(body.value("names").toArray().first().toString(), QStringLiteral("diffusion_models/z.gguf"));
        QCOMPARE(list.hashing, 1); // the host says it is hashing
    }

    void staleMeansNeverFetchedOrOlderThanTheTtl() {
        HostModelList list;
        QVERIFY(HostModelCache::stale(list, 300));
        list.fetchedAt = QDateTime::currentDateTimeUtc().addSecs(-100);
        QVERIFY(!HostModelCache::stale(list, 300));
        list.fetchedAt = QDateTime::currentDateTimeUtc().addSecs(-400);
        QVERIFY(HostModelCache::stale(list, 300));
    }

    void refreshStaleAsksOnlyThePairedHelpersWhoseListIsOld() {
        answer(reply(QJsonArray{entry("vae/ae.safetensors", 300, kHash)}));
        HostModelCache kept = cache();
        QDir(m_dir.filePath("kept")).removeRecursively();
        const int before = int(m_host.requestsTo("POST", "/fugue/v1/models").size());
        ProviderInstance local; // not a paired helper: no address
        local.name = QStringLiteral("Fugue helper");
        local.type = QStringLiteral("fugue-helper");
        int changed = 0;
        kept.refreshStale({local, instance()}, 300, [&changed](const HostModelList &) { ++changed; }, reach());
        QTRY_COMPARE_WITH_TIMEOUT(changed, 1, 5000);
        QCOMPARE(int(m_host.requestsTo("POST", "/fugue/v1/models").size()), before + 1);
        kept.refreshStale({instance()}, 300, [&changed](const HostModelList &) { ++changed; }, reach()); // fresh now: no request
        QTest::qWait(50);
        QCOMPARE(int(m_host.requestsTo("POST", "/fugue/v1/models").size()), before + 1);
        QCOMPARE(changed, 1);
    }

    void aSecondRefreshWhileOneIsOutJoinsIt() {
        answer(reply(QJsonArray{entry("vae/ae.safetensors", 300, kHash)}));
        HostModelCache kept = cache();
        const int before = int(m_host.requestsTo("POST", "/fugue/v1/models").size());
        int done = 0;
        kept.refresh(instance(), false, {}, [&done](const HostModelList &) { ++done; }, reach());
        kept.refresh(instance(), false, {}, [&done](const HostModelList &) { ++done; }, reach());
        QTRY_COMPARE_WITH_TIMEOUT(done, 2, 5000);
        QCOMPARE(int(m_host.requestsTo("POST", "/fugue/v1/models").size()), before + 1);
    }

    void hostsHavingAllAreTheOnesThatListEveryHash() {
        answer(reply(QJsonArray{entry("diffusion_models/z.gguf", 4000, kHash), entry("vae/ae.safetensors", 300, QString(64, 'c'))}));
        HostModelCache kept = cache();
        refreshed(kept);
        ProviderInstance other = instance(QStringLiteral("spark")); // same address, its own list: none kept
        const QString one = "sha256:" + kHash, two = "sha256:" + QString(64, 'c');
        QCOMPARE(kept.hostsHavingAll({other, instance()}, {one, two}), QStringList{QStringLiteral("gx10")});
        QCOMPARE(kept.hostsHavingAll({instance()}, {one, "sha256:" + QString(64, 'd')}), QStringList());
        QCOMPARE(kept.availability({instance()}, {QJsonObject{{"hash", one}}}).host, QStringLiteral("gx10"));
        QCOMPARE(kept.availability({instance()}, {QJsonObject{{"name", "x"}}}).host, QString()); // no hash: no host
        ProviderInstance disabled = instance();
        disabled.enabled = false;
        QVERIFY(kept.hostsHavingAll({disabled}, {one}).isEmpty());
    }

    // #735 case a: gx10 has the diffusion model but not the VAE the same nodes need: it does not count, and says why.
    void aHostCountsOnlyWhenItHasEveryMissingModel() {
        answer(reply(QJsonArray{entry("diffusion_models/z.gguf", 4000, kHash)}));
        HostModelCache kept = cache();
        refreshed(kept);
        const QJsonObject diffusion{{"hash", "sha256:" + kHash}}, vae{{"hash", "sha256:" + QString(64, 'f')}};
        QCOMPARE(kept.availability({instance()}, {diffusion}).host, QStringLiteral("gx10"));
        const HostAvailability both = kept.availability({instance()}, {diffusion, vae});
        QVERIFY(both.host.isEmpty());
        QVERIFY2(both.why.contains("gx10 has it, but"), qPrintable(both.why));
    }

    // Case b: a list whose last refresh failed, or older than 10 minutes, does not make a model available.
    void aListWithAnErrorOrTooOldDoesNotCount() {
        answer(reply(QJsonArray{entry("diffusion_models/z.gguf", 4000, kHash)}));
        HostModelCache kept = cache();
        refreshed(kept);
        const QJsonObject diffusion{{"hash", "sha256:" + kHash}};
        QVERIFY(kept.availability({instance()}, {diffusion}, QDateTime::currentDateTimeUtc().addSecs(700)).why.contains("older than 10 minutes"));
        m_host.on("POST", "/fugue/v1/models", [](const HttpRequest &) {
            return HttpResponse{500, json({{"error", QJsonObject{{"code", "helper_failed"}, {"message", "boom"}}}})};
        });
        refreshed(kept);
        const HostAvailability failed = kept.availability({instance()}, {diffusion});
        QVERIFY(failed.host.isEmpty());
        QVERIFY2(failed.why.contains("could not be refreshed"), qPrintable(failed.why));
        QVERIFY(!HostModelCache::stale(kept.cached(instance()), 300)); // a failed try counts: a host that is down is not asked in a loop
    }

    // A request to hash that comes while a refresh is out is sent after it, never dropped.
    void aHashRequestDuringARefreshIsSentAfterIt() {
        answer(reply(QJsonArray{entry("diffusion_models/z.gguf", 4000, "")}, 0));
        HostModelCache kept = cache();
        const int before = int(m_host.requestsTo("POST", "/fugue/v1/models").size());
        int plain = 0, hashed = 0;
        kept.refresh(instance(), false, {}, [&plain](const HostModelList &) { ++plain; }, reach());
        kept.refresh(instance(), true, {QStringLiteral("diffusion_models/z.gguf")}, [&hashed](const HostModelList &) { ++hashed; }, reach());
        QTRY_VERIFY_WITH_TIMEOUT(plain == 1 && hashed == 1, 5000);
        const QList<HttpRequest> sent = m_host.requestsTo("POST", "/fugue/v1/models");
        QCOMPARE(int(sent.size()), before + 2);
        QVERIFY(!QJsonDocument::fromJson(sent.at(before).body).object().contains("hash"));
        const QJsonObject second = QJsonDocument::fromJson(sent.at(before + 1).body).object();
        QCOMPARE(second.value("hash").toBool(), true);
        QCOMPARE(second.value("names").toArray().first().toString(), QStringLiteral("diffusion_models/z.gguf"));
    }

    // What a host says is data: names that step out of a folder or carry control characters are dropped, a huge list is cut.
    void aHostsListIsSanitised() {
        QJsonArray many;
        for (const QString &bad : {QStringLiteral("../etc/passwd"), QStringLiteral("/abs/x.gguf"), QStringLiteral("a\\b.gguf"), QStringLiteral("vae/\x1b[31mred.gguf"), QString(600, 'a')})
            many.append(entry(bad, 1, kHash));
        many.append(entry("vae/ok.safetensors", 1, kHash, "Fam\x1bily"));
        const HostModelList list = HostModelList::fromJson(QJsonObject{{"models", many}});
        QCOMPARE(list.models.size(), 1);
        QCOMPARE(list.models.first().name, QStringLiteral("vae/ok.safetensors"));
        QVERIFY(!list.models.first().family.contains(QChar(0x1b)));
        QJsonArray huge;
        for (int i = 0; i < 4100; ++i)
            huge.append(entry(QStringLiteral("vae/%1.safetensors").arg(i), 1, ""));
        QCOMPARE(HostModelList::fromJson(QJsonObject{{"models", huge}}).models.size(), 4000);
    }

    // The picker's group: files under one of the type's folders, as remote rows with the handle a pick stores.
    void theModelPickerListsTheHostsFilesOfTheTypeAsRemoteRows() {
        answer(reply(QJsonArray{entry("diffusion_models/z-image-turbo-Q8_0.gguf", 4000, kHash), entry("vae/ae.safetensors", 300, ""),
                                entry("loras/x.safetensors", 10, QString(64, 'e'))}));
        HostModelCache kept = cache();
        refreshed(kept);
        Registry registry;
        for (const auto &[id, folders] : {std::pair{"fugue/diffusion-model", QStringList{"diffusion_models", "unet"}}, std::pair{"fugue/vae", QStringList{"vae"}}}) {
            DeclaredPortType declared;
            declared.type = *PortType::parse(QString::fromLatin1(id));
            declared.kind = PortKind::Resource;
            declared.name = QStringLiteral("A model");
            declared.folders = folders;
            QVERIFY(registry.add(declared).isEmpty());
        }
        const ModelLibrary library({m_dir.filePath("models")}, &registry);
        // The one rule for both pickers: a host file with a hash that a local file has is left out, an unhashed one with a local file's
        // name and size says "Also here".
        ListedModel here;
        here.name = QStringLiteral("z-image-turbo-Q8_0.gguf");
        here.size = 4000;
        here.handle = QJsonObject{{"hash", "sha256:" + kHash}};
        QCOMPARE(listHostModels({instance()}, kept, library, *PortType::parse("fugue/diffusion-model"), {here}).size(), 0); // same hash: local wins
        ListedModel sameName = here;
        sameName.handle = QJsonObject{{"hash", "sha256:" + QString(64, '9')}};
        QCOMPARE(listHostModels({instance()}, kept, library, *PortType::parse("fugue/diffusion-model"), {sameName}).size(), 1); // another content: both stay
        ListedModel unhashedHere;
        unhashedHere.name = QStringLiteral("ae.safetensors");
        unhashedHere.size = 300;
        const QList<ListedModel> also = listHostModels({instance()}, kept, library, *PortType::parse("fugue/vae"), {unhashedHere});
        QCOMPARE(also.size(), 1);
        QVERIFY(also.first().note.contains("Also here"));
        const QList<ListedModel> unets = listHostModels({instance()}, kept, library, *PortType::parse("fugue/diffusion-model"));
        QCOMPARE(unets.size(), 1);
        QVERIFY(unets.first().remote);
        QCOMPARE(unets.first().host, QStringLiteral("gx10"));
        QCOMPARE(unets.first().name, QStringLiteral("z-image-turbo-Q8_0.gguf"));
        QCOMPARE(unets.first().handle.value("hash").toString(), "sha256:" + kHash);
        QCOMPARE(unets.first().handle.value("resource").toString(), QStringLiteral("fugue/diffusion-model"));
        QVERIFY(!unets.first().family.isEmpty()); // the table names it from the file name
        const QList<ListedModel> vaes = listHostModels({instance()}, kept, library, *PortType::parse("fugue/vae"));
        QCOMPARE(vaes.size(), 1);
        QVERIFY(!vaes.first().handle.contains("hash")); // not hashed on the host yet
        QVERIFY(vaes.first().note.contains("hashed"));
    }
};

QTEST_MAIN(TestHostModels)
#include "test_host_models.moc"
