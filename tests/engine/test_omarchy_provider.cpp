// The omarchy provider (spec 8.7) against a fake sd-server and fake omarchy-local-ai state
// files in a temporary directory. Nothing here reads ~ or talks to a real server.
//
// Fixture shapes follow omarchy-local-ai origin/master 703ae0c (agent_local.py model_record,
// clean_listing, remote_record; docs/engine.md "State and panel API", "Tailnet sharing") and
// sd-server at stable-diffusion.cpp 532d2c15 (examples/server/api.md, async_jobs.cpp
// make_async_job_json).

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/omarchy_provider.h"
#include "fake_http.h"
#include "provider_support.h"

using namespace fugue;
using fugue::test::await;
using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

const QByteArray kKey = "local-key-0123456789";
const QString kKlein = QStringLiteral("flux2-klein-4b-sdcpp");
const QString kZImage = QStringLiteral("z-image-turbo-sdcpp");

QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

// A status.json model row with the fields Fugue reads.
QJsonObject row(const QString &recipe, const QString &kind, const QString &unit, const QString &endpoint) {
    return QJsonObject{{"recipe", recipe}, {"kind", kind}, {"edit", false}, {"name", recipe},
                       {"vendor", ""}, {"runtime", kind == "image" ? "sdcpp" : "gufo"},
                       {"unit", unit}, {"endpoint", endpoint}, {"fits", true}, {"fit_reason", ""},
                       {"error", ""}};
}

QJsonObject capabilities() {
    return QJsonObject{
        {"supported_modes", QJsonArray{"img_gen"}},
        {"defaults_by_mode",
         QJsonObject{{"img_gen",
                      QJsonObject{{"width", 1024}, {"height", 1024}, {"seed", -1},
                                  {"sample_params",
                                   QJsonObject{{"sample_steps", 4},
                                               {"guidance", QJsonObject{{"txt_cfg", 1.0}}}}}}}}},
        {"limits", QJsonObject{{"min_width", 64}, {"max_width", 4096}, {"min_height", 64},
                               {"max_height", 4096}, {"max_batch_count", 8}, {"max_queue_size", 64}}},
        {"loras", QJsonArray{}},
    };
}

const OfferedModel *findModel(const Description &description, const QString &capability, const QString &name) {
    const CapabilityOffer *offer = description.offer(capability);
    if (!offer)
        return nullptr;
    for (const OfferedModel &model : offer->models)
        if (model.handle.value("name").toString() == name)
            return &model;
    return nullptr;
}

class Fixture {
public:
    Fixture() {
        server.listen();
        paths.stateDir = home.filePath("state");
        paths.configDir = home.filePath("config");
        writeFile(QDir(paths.configDir).filePath("key"), kKey + "\n");
        server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &request) {
            if (request.headers.value("authorization") != "Bearer " + kKey)
                return HttpResponse{401, "{\"error\":\"unauthorized\"}"};
            return HttpResponse{200, json(capabilities())};
        });
    }

    QString endpoint() const { return QString::fromLatin1(server.origin()) + "/v1"; }

    void writeStatus(const QJsonObject &models) {
        writeFile(QDir(paths.stateDir).filePath("status.json"),
                  json(QJsonObject{{"models", models}, {"last", QJsonValue::Null}, {"ram_total_gib", 128}}));
    }

    std::unique_ptr<OmarchyProvider> provider(ProviderInstance instance = {}) {
        if (instance.name.isEmpty())
            instance.name = QStringLiteral("This machine");
        instance.type = QStringLiteral("omarchy");
        instance.settings.insert("poll_ms", 10);
        return std::make_unique<OmarchyProvider>(instance, paths);
    }

    QTemporaryDir home;
    OmarchyPaths paths;
    FakeHttpServer server;
};

} // namespace

class TestOmarchyProvider : public QObject {
    Q_OBJECT

private slots:
    void describeRoutesImageModelsByKindAndEdit() {
        Fixture fixture;
        QJsonObject klein = row(kKlein, "image", "active", fixture.endpoint());
        klein.insert("edit", true);
        QJsonObject zimage = row(kZImage, "image", "inactive", fixture.endpoint());
        zimage.insert("fits", false);
        zimage.insert("fit_reason", "Needs 13 GiB of free memory; 6 GiB is free");
        fixture.writeStatus(QJsonObject{
            {kKlein, klein},
            {kZImage, zimage},
            {"qwen3.8-27b-gufo", row("qwen3.8-27b-gufo", "text", "active", fixture.endpoint())},
        });
        auto provider = fixture.provider();

        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(described.ok());
        QCOMPARE(described.value.engine, QStringLiteral("sdcpp"));
        QVERIFY(!described.value.offer("text.generate"));

        const OfferedModel *ready = findModel(described.value, "image.generate", kKlein);
        QVERIFY(ready);
        QCOMPARE(ready->status, QStringLiteral("ready"));
        QCOMPARE(ready->handle, (QJsonObject{{"resource", "fugue/image-model"}, {"name", kKlein}}));
        QCOMPARE(ready->properties.value("edit").toBool(), true);
        QCOMPARE(ready->defaults.value("steps").toInt(), 4);
        QCOMPARE(ready->defaults.value("guidance").toDouble(), 1.0);
        QCOMPARE(ready->defaults.value("width").toInt(), 1024);
        QCOMPARE(ready->limits.value("max_width").toInt(), 4096);
        QVERIFY(findModel(described.value, "image.edit", kKlein));

        const OfferedModel *stopped = findModel(described.value, "image.generate", kZImage);
        QVERIFY(stopped);
        QCOMPARE(stopped->status, QStringLiteral("unavailable"));
        QVERIFY(stopped->reason.contains("13 GiB"));
        QVERIFY(stopped->hint.contains("omarchy-agent-local start " + kZImage));
        QVERIFY(!findModel(described.value, "image.edit", kZImage));

        const auto asked = fixture.server.requestsTo("GET", "/sdcpp/v1/capabilities");
        QCOMPARE(asked.size(), 1); // only the running server is asked
        QCOMPARE(asked.first().headers.value("authorization"), "Bearer " + kKey);
    }

    void aStartingModelIsLoading() {
        Fixture fixture;
        fixture.writeStatus(QJsonObject{{kKlein, row(kKlein, "image", "activating", fixture.endpoint())}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QCOMPARE(findModel(described.value, "image.generate", kKlein)->status, QStringLiteral("loading"));
        QCOMPARE(described.value.offer("image.generate")->status(), QStringLiteral("loading"));
    }

    void aRunningServerStillLoadingItsModelIsLoading() {
        Fixture fixture;
        fixture.server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) {
            return HttpResponse{503, "{\"error\":\"Model is loading\",\"code\":\"model_loading\"}"};
        });
        fixture.writeStatus(QJsonObject{{kKlein, row(kKlein, "image", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QCOMPARE(findModel(described.value, "image.generate", kKlein)->status, QStringLiteral("loading"));
    }

    void aRunningServerThatDoesNotAnswerIsUnavailable() {
        Fixture fixture;
        quint16 closed;
        {
            QTcpServer probe;
            probe.listen(QHostAddress::LocalHost, 0);
            closed = probe.serverPort();
        }
        fixture.writeStatus(QJsonObject{
            {kKlein, row(kKlein, "image", "active", QStringLiteral("http://127.0.0.1:%1/v1").arg(closed))}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(described.ok());
        const OfferedModel *model = findModel(described.value, "image.generate", kKlein);
        QCOMPARE(model->status, QStringLiteral("unavailable"));
        QVERIFY(model->reason.contains("not answering"));
    }

    void withoutStatusJsonOmarchyIsNotSetUp() {
        Fixture fixture;
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!described.ok());
        QCOMPARE(described.errors.first().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.errors.first().hint.contains("omarchy-agent-local"));
    }

    void withoutAKeyNothingCanBeAsked() {
        Fixture fixture;
        QFile::remove(QDir(fixture.paths.configDir).filePath("key"));
        fixture.writeStatus(QJsonObject{{kKlein, row(kKlein, "image", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!described.ok());
        QCOMPARE(described.errors.first().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.errors.first().hint.contains("key"));
    }

    void aRefusedKeySaysSo() {
        Fixture fixture;
        fixture.writeStatus(QJsonObject{{kKlein, row(kKlein, "image", "active", fixture.endpoint())}});
        ProviderInstance instance;
        instance.key = QStringLiteral("wrong-key");
        auto provider = fixture.provider(instance);
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        const OfferedModel *model = findModel(described.value, "image.generate", kKlein);
        QCOMPARE(model->status, QStringLiteral("unavailable"));
        QVERIFY(model->reason.contains("key"));
        QCOMPARE(fixture.server.requests().first().headers.value("authorization"), QByteArray("Bearer wrong-key"));
    }

    void aPairedHostIsReadFromNetworkJsonWithItsOwnKey() {
        Fixture fixture;
        const QByteArray remoteKey = "studio-key-0123456789";
        writeFile(QDir(fixture.paths.configDir).filePath("remotes/studio.key"), remoteKey);
        // The share proxy serves https://<dns>/local-ai/<recipe>/... (docs/engine.md "Tailnet
        // sharing"), so the server's base keeps that path.
        fixture.server.on("GET", "/local-ai/" + kKlein.toLatin1() + "/sdcpp/v1/capabilities",
                          [remoteKey](const HttpRequest &request) {
                              if (request.headers.value("authorization") != "Bearer " + remoteKey)
                                  return HttpResponse{401, "{}"};
                              return HttpResponse{200, json(capabilities())};
                          });
        const QString dns = QStringLiteral("studio.tail1234.ts.net");
        auto remote = [&](const QString &recipe, bool paired, const QString &offline) {
            QJsonObject model = row(recipe, "image", "active",
                                    QString::fromLatin1(fixture.server.origin()) + "/local-ai/" + recipe + "/v1");
            model.insert("id", "studio/" + recipe);
            model.insert("host", "studio");
            model.insert("dns", dns);
            model.insert("paired", paired);
            model.insert("offline", offline);
            model.insert("edit", true);
            model.insert("input", QJsonArray{"text"});
            return model;
        };
        QJsonObject other = remote("other-sdcpp", true, "");
        other.insert("dns", "other.tail1234.ts.net");
        writeFile(QDir(fixture.paths.stateDir).filePath("network.json"),
                  json(QJsonObject{{"schema", 1},
                                   {"models", QJsonObject{
                                                  {"studio/" + kKlein, remote(kKlein, true, "")},
                                                  {"studio/" + kZImage, remote(kZImage, true, "offline")},
                                                  {"studio/unpaired-sdcpp", remote("unpaired-sdcpp", false, "")},
                                                  {"other/other-sdcpp", other},
                                              }}}));
        ProviderInstance instance;
        instance.name = QStringLiteral("Studio box");
        instance.address = QStringLiteral("https://") + dns;
        auto provider = fixture.provider(instance);

        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(described.ok());
        QCOMPARE(described.value.offer("image.generate")->models.size(), 2);
        QCOMPARE(findModel(described.value, "image.generate", kKlein)->status, QStringLiteral("ready"));
        QCOMPARE(findModel(described.value, "image.edit", kKlein)->defaults.value("steps").toInt(), 4);
        const OfferedModel *offline = findModel(described.value, "image.generate", kZImage);
        QCOMPARE(offline->status, QStringLiteral("unavailable"));
        QVERIFY(offline->reason.contains("offline"));
    }

    void anUnpairedHostSaysHowToPair() {
        Fixture fixture;
        ProviderInstance instance;
        instance.name = QStringLiteral("Studio box");
        instance.address = QStringLiteral("https://studio.tail1234.ts.net");
        auto provider = fixture.provider(instance);
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!described.ok());
        QCOMPARE(described.errors.first().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.errors.first().hint.contains("omarchy-agent-local pair studio"));
    }
};

QTEST_GUILESS_MAIN(TestOmarchyProvider)
#include "test_omarchy_provider.moc"
