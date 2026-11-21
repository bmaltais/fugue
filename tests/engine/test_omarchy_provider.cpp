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
#include "engine/providers.h"
#include "fake_http.h"
#include "omarchy_fixture.h"
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
const QString kVideoModel = QStringLiteral("minimax-h3-sdcpp");
const QString kMusicModel = QStringLiteral("ace-step-1.5-turbo-acestep");

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
        instance.settings.insert("poll_ms", pollMs);
        return std::make_unique<OmarchyProvider>(instance, paths);
    }

    // How often the providers of this fixture poll their jobs (the `poll_ms` setting).
    static constexpr int pollMs = 10;

    // Klein (an edit model) running on the fake server.
    void runKlein() {
        QJsonObject klein = row(kKlein, "image", "active", endpoint());
        klein.insert("edit", true);
        writeStatus(QJsonObject{{kKlein, klein}});
    }

    // img_gen answers 202 with this job id (api.md "POST /sdcpp/v1/img_gen").
    void acceptJobs(const QByteArray &id) {
        server.on("POST", "/sdcpp/v1/img_gen", [id](const HttpRequest &) {
            return HttpResponse{202, json(QJsonObject{{"id", QString::fromLatin1(id)}, {"kind", "img_gen"},
                                                      {"status", "queued"}, {"created", qint64(1775401200000)},
                                                      {"poll_url", "/sdcpp/v1/jobs/" + QString::fromLatin1(id)}})};
        });
    }

    // Polls of the job get these answers in turn; after the last, 410 Gone as the server does
    // once a finished job has expired (api.md "GET /sdcpp/v1/jobs/{id}").
    void scriptJob(const QByteArray &id, QList<QJsonObject> answers) {
        auto queue = std::make_shared<QList<QJsonObject>>(std::move(answers));
        server.on("GET", "/sdcpp/v1/jobs/" + id, [queue](const HttpRequest &) {
            if (queue->isEmpty())
                return HttpResponse{410, "{\"error\":\"gone\"}"};
            return HttpResponse{200, json(queue->takeFirst())};
        });
    }

    JobRequest request(const QString &capability, const QJsonObject &params, const QJsonObject &inputs = {}) {
        JobRequest job;
        job.capability = capability;
        job.params = params;
        job.inputs = inputs;
        job.outputDir = home.filePath("out");
        QDir().mkpath(job.outputDir);
        return job;
    }

    QString writeInput(const QString &name, const QByteArray &bytes) {
        const QString path = home.filePath(name);
        writeFile(path, bytes);
        return path;
    }

    QTemporaryDir home;
    OmarchyPaths paths;
    FakeHttpServer server;
};

// A job as make_async_job_json writes it (async_jobs.cpp).
QJsonObject job(const QString &status, const QJsonObject &extra = {}) {
    QJsonObject object{{"id", "job_1"}, {"kind", "img_gen"}, {"status", status},
                       {"created", qint64(1775401200000)}, {"queue_position", 0},
                       {"result", QJsonValue::Null}, {"error", QJsonValue::Null},
                       {"progress", QJsonObject{{"stage", status == "queued" ? "queued" : "preparing"},
                                                {"detail", ""}, {"pass", 0},
                                                {"completed", QJsonValue::Null}, {"total", QJsonValue::Null},
                                                {"unit", QJsonValue::Null}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        object.insert(it.key(), it.value());
    return object;
}

QJsonObject sampling(int completed, int total, const QByteArray &preview = {}) {
    QJsonObject extra{{"progress", QJsonObject{{"stage", "sampling"}, {"detail", ""}, {"pass", 1},
                                               {"completed", completed}, {"total", total}, {"unit", "steps"}}}};
    if (!preview.isEmpty())
        extra.insert("preview", QJsonObject{{"pass", 1}, {"step", completed}, {"total_steps", total},
                                            {"b64_json", QString::fromLatin1(preview.toBase64())}});
    return job("generating", extra);
}

QJsonObject completed(const QByteArray &png, const QJsonObject &generationInfo) {
    return job("completed",
               {{"result", QJsonObject{{"output_format", "png"},
                                       {"images", QJsonArray{QJsonObject{
                                                      {"index", 0},
                                                      {"b64_json", QString::fromLatin1(png.toBase64())},
                                                      {"generation_info", generationInfo}}}}}}});
}

QJsonObject requestBody(const FakeHttpServer &server) {
    const auto posts = server.requestsTo("POST", "/sdcpp/v1/img_gen");
    return posts.isEmpty() ? QJsonObject{} : QJsonDocument::fromJson(posts.last().body).object();
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

const QString kQwen = QStringLiteral("qwen3.8-27b-gufo");

// An OpenAI-compatible /v1/models answer listing the recipe (omarchy-local-ai's post-start check).
HttpResponse modelsList(const HttpRequest &request, const QString &recipe) {
    if (request.headers.value("authorization") != "Bearer " + kKey)
        return HttpResponse{401, "{\"error\":\"unauthorized\"}"};
    return HttpResponse{200, json(QJsonObject{{"object", "list"},
                                              {"data", QJsonArray{QJsonObject{{"id", recipe}, {"object", "model"}}}}})};
}

// A streamed chat completion (server-sent events): one chunk per piece of the answer, after
// one of reasoning that is not part of it, then the finish and [DONE].
QByteArray streamed(const QStringList &pieces, const QString &reasoning = {}) {
    QByteArray body;
    const auto chunk = [&body](const QJsonObject &delta, const QJsonValue &finish) {
        body += "data: " +
                json(QJsonObject{{"id", "chatcmpl-1"}, {"object", "chat.completion.chunk"},
                                 {"choices", QJsonArray{QJsonObject{{"index", 0}, {"delta", delta},
                                                                    {"finish_reason", finish}}}}}) +
                "\n\n";
    };
    chunk(QJsonObject{{"role", "assistant"}, {"content", ""}}, QJsonValue::Null);
    if (!reasoning.isEmpty())
        chunk(QJsonObject{{"reasoning_content", reasoning}}, QJsonValue::Null);
    for (const QString &piece : pieces)
        chunk(QJsonObject{{"content", piece}}, QJsonValue::Null);
    chunk(QJsonObject{}, QStringLiteral("stop"));
    return body + "data: [DONE]\n\n";
}

QJsonObject chatBody(const FakeHttpServer &server) {
    const auto posts = server.requestsTo("POST", "/v1/chat/completions");
    return posts.isEmpty() ? QJsonObject{} : QJsonDocument::fromJson(posts.last().body).object();
}

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
        QCOMPARE(described.value().engine, QStringLiteral("sdcpp"));
        QVERIFY(!findModel(described.value(), "image.generate", "qwen3.8-27b-gufo"));

        const OfferedModel *ready = findModel(described.value(), "image.generate", kKlein);
        QVERIFY(ready);
        QCOMPARE(ready->status, ModelStatus::Ready);
        QCOMPARE(ready->handle, (QJsonObject{{"resource", "fugue/image-model"}, {"name", kKlein}}));
        QCOMPARE(ready->properties.value("edit").toBool(), true);
        QCOMPARE(ready->defaults.value("steps").toInt(), 4);
        QCOMPARE(ready->defaults.value("guidance").toDouble(), 1.0);
        QCOMPARE(ready->defaults.value("width").toInt(), 1024);
        QCOMPARE(ready->limits.value("max_width").toInt(), 4096);
        QVERIFY(findModel(described.value(), "image.edit", kKlein));

        const OfferedModel *stopped = findModel(described.value(), "image.generate", kZImage);
        QVERIFY(stopped);
        QCOMPARE(stopped->status, ModelStatus::Unavailable);
        QVERIFY(stopped->reason.contains("13 GiB"));
        QVERIFY(stopped->hint.contains("omarchy-agent-local start " + kZImage));
        QVERIFY(!findModel(described.value(), "image.edit", kZImage));

        const auto asked = fixture.server.requestsTo("GET", "/sdcpp/v1/capabilities");
        QCOMPARE(asked.size(), 1); // only the running server is asked
        QCOMPARE(asked.first().headers.value("authorization"), "Bearer " + kKey);
    }

    void aStartingModelIsLoading() {
        Fixture fixture;
        fixture.writeStatus(QJsonObject{{kKlein, row(kKlein, "image", "activating", fixture.endpoint())}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QCOMPARE(findModel(described.value(), "image.generate", kKlein)->status, ModelStatus::Loading);
        QCOMPARE(described.value().offer("image.generate")->status(), ModelStatus::Loading);
    }

    void aRunningServerStillLoadingItsModelIsLoading() {
        Fixture fixture;
        fixture.server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) {
            return HttpResponse{503, "{\"error\":\"Model is loading\",\"code\":\"model_loading\"}"};
        });
        fixture.writeStatus(QJsonObject{{kKlein, row(kKlein, "image", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QCOMPARE(findModel(described.value(), "image.generate", kKlein)->status, ModelStatus::Loading);
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
        const OfferedModel *model = findModel(described.value(), "image.generate", kKlein);
        QCOMPARE(model->status, ModelStatus::Unavailable);
        QVERIFY(model->reason.contains("not answering"));
    }

    void withoutStatusJsonOmarchyIsNotSetUp() {
        Fixture fixture;
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!described.ok());
        QCOMPARE(described.errors().first().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.errors().first().hint.contains("omarchy-agent-local"));
    }

    void evenAnImmediateFailureRepliesFromTheEventLoop() {
        Fixture fixture;
        auto provider = fixture.provider();
        bool described = false;
        bool submitted = false;
        provider->describe([&](const Result<Description> &) { described = true; });
        provider->submit(fixture.request("image.generate", {{"prompt", "x"}}),
                         [&](const Result<QString> &) { submitted = true; });
        QVERIFY(!described);
        QVERIFY(!submitted);
        QTRY_VERIFY(described && submitted);
    }

    void withoutAKeyNothingCanBeAsked() {
        Fixture fixture;
        QFile::remove(QDir(fixture.paths.configDir).filePath("key"));
        fixture.writeStatus(QJsonObject{{kKlein, row(kKlein, "image", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!described.ok());
        QCOMPARE(described.errors().first().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.errors().first().hint.contains("key"));
    }

    void aRefusedKeySaysSo() {
        Fixture fixture;
        fixture.writeStatus(QJsonObject{{kKlein, row(kKlein, "image", "active", fixture.endpoint())}});
        ProviderInstance instance;
        instance.key = QStringLiteral("wrong-key");
        auto provider = fixture.provider(instance);
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        const OfferedModel *model = findModel(described.value(), "image.generate", kKlein);
        QCOMPARE(model->status, ModelStatus::Unavailable);
        QVERIFY(model->reason.contains("key"));
        QCOMPARE(fixture.server.requests().first().headers.value("authorization"), QByteArray("Bearer wrong-key"));
    }

    // #72: sd-server reports 0 for a size it leaves to the model ("native"): that is no
    // default, not a size of 0, so it is not passed on (and a run does not fail on it).
    void aDefaultSizeOfZeroIsNoDefault() {
        Fixture fixture;
        fixture.server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) {
            QJsonObject caps = capabilities();
            QJsonObject modes = caps.value("defaults_by_mode").toObject();
            QJsonObject imgGen = modes.value("img_gen").toObject();
            imgGen.insert("width", 0);
            imgGen.insert("height", 0);
            modes.insert("img_gen", imgGen);
            caps.insert("defaults_by_mode", modes);
            return HttpResponse{200, json(caps)};
        });
        fixture.writeStatus(QJsonObject{{kKlein, row(kKlein, "image", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(described.ok());
        const OfferedModel *model = findModel(described.value(), "image.generate", kKlein);
        QVERIFY(model);
        QVERIFY(!model->defaults.contains("width"));
        QVERIFY(!model->defaults.contains("height"));
        QCOMPARE(model->defaults.value("steps").toInt(), 4);
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
        // network.json is rewritten only while the agents panel is open, so its unit can be
        // stale: a model started since says inactive. The server's own answer decides.
        QJsonObject started = remote(kKlein, true, "");
        started.insert("unit", "inactive");
        // The share proxy answers 502 when the model's port refuses: it is stopped.
        fixture.server.on("GET", "/local-ai/stopped-sdcpp/sdcpp/v1/capabilities",
                          [](const HttpRequest &) { return HttpResponse{502, ""}; });
        QJsonObject other = remote("other-sdcpp", true, "");
        other.insert("dns", "other.tail1234.ts.net");
        writeFile(QDir(fixture.paths.stateDir).filePath("network.json"),
                  json(QJsonObject{{"schema", 1},
                                   {"models", QJsonObject{
                                                  {"studio/" + kKlein, started},
                                                  {"studio/stopped-sdcpp", remote("stopped-sdcpp", true, "")},
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
        QCOMPARE(described.value().offer("image.generate")->models.size(), 3);
        QCOMPARE(findModel(described.value(), "image.generate", kKlein)->status, ModelStatus::Ready);
        QCOMPARE(findModel(described.value(), "image.edit", kKlein)->defaults.value("steps").toInt(), 4);
        const OfferedModel *offline = findModel(described.value(), "image.generate", kZImage);
        QCOMPARE(offline->status, ModelStatus::Unavailable);
        QVERIFY(offline->reason.contains("offline"));
        const OfferedModel *stopped = findModel(described.value(), "image.generate", "stopped-sdcpp");
        QCOMPARE(stopped->status, ModelStatus::Unavailable);
        QVERIFY(stopped->hint.contains("on studio: omarchy-agent-local start stopped-sdcpp"));
    }

    void theDefaultInstancesOfPairedHostsServeTheirModels() {
        // No providers.json: the instances of spec 8.2's default, on state as a real machine
        // paired with a host has it.
        QTemporaryDir home;
        FakeHttpServer server;
        server.listen();
        const OmarchyPaths paths = test::writeOmarchyState(home.path(), QString::fromLatin1(server.origin()));
        test::serveStudio(server);
        const QList<ProviderInstance> instances = defaultProviderInstances(paths);
        QCOMPARE(instances.size(), 3);

        OmarchyProvider studio(instances.at(2), paths);
        const auto described = await<Description>([&](auto reply) { studio.describe(reply); });
        QVERIFY2(described.ok(), qPrintable(described.ok() ? QString() : described.error().message));
        QCOMPARE(described.value().offer("image.generate")->models.size(), 2);
        const OfferedModel *klein = findModel(described.value(), "image.generate", "flux2-klein-4b-sdcpp-cuda");
        QCOMPARE(klein->status, ModelStatus::Ready);
        QCOMPARE(klein->title, QStringLiteral("FLUX.2 Klein 4B"));
        QVERIFY(!klein->handle.contains("title")); // display only: not part of the handle (2.5)
        QCOMPARE(described.value().offer("image.edit")->models.size(), 1);
        QVERIFY(findModel(described.value(), "image.edit", "flux2-klein-4b-sdcpp-cuda"));
        // Its text model, with what it takes and how long an answer may be (network.json).
        const OfferedModel *qwen = findModel(described.value(), "text.generate", "qwen3.5-2b-mlx-4bit-tensorfold");
        QVERIFY(qwen);
        QCOMPARE(qwen->status, ModelStatus::Ready);
        QCOMPARE(qwen->title, QStringLiteral("Qwen3.5 2B"));
        QCOMPARE(qwen->handle, (QJsonObject{{"resource", "fugue/text-model"}, {"name", "qwen3.5-2b-mlx-4bit-tensorfold"}}));
        QCOMPARE(qwen->properties.value("input").toArray(), (QJsonArray{"text", "image"}));
        QCOMPARE(qwen->limits.value("max_tokens").toInt(), 8192);
        QVERIFY(!findModel(described.value(), "image.generate", "qwen3.5-2b-mlx-4bit-tensorfold"));

        // The offline host describes itself: its last-seen model, as unavailable and why.
        OmarchyProvider lab(instances.at(1), paths);
        const auto away = await<Description>([&](auto reply) { lab.describe(reply); });
        QVERIFY(away.ok());
        const OfferedModel *zimage = findModel(away.value(), "image.generate", "z-image-turbo-sdcpp-cuda");
        QCOMPARE(zimage->status, ModelStatus::Unavailable);
        QVERIFY(zimage->reason.contains("lab is offline"));
    }

    void aPairedHostWithNoModelSaysWhyNotThatItIsUnpaired() {
        Fixture fixture;
        writeFile(QDir(fixture.paths.configDir).filePath("remotes/studio.key"), "studio-key");
        auto network = [&](const QString &state) {
            writeFile(QDir(fixture.paths.stateDir).filePath("network.json"),
                      json(QJsonObject{{"schema", 1},
                                       {"hosts", QJsonObject{{"studio", QJsonObject{{"host", "studio"},
                                                                                    {"dns", "studio.tail1234.ts.net"},
                                                                                    {"paired", true},
                                                                                    {"state", state}}}}},
                                       {"models", QJsonObject{}}}));
        };
        ProviderInstance instance;
        instance.name = QStringLiteral("studio");
        instance.address = QStringLiteral("https://studio.tail1234.ts.net");
        auto provider = fixture.provider(instance);

        network("online");
        auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!described.ok());
        QCOMPARE(described.errors().first().code, QStringLiteral("provider_unreachable"));
        QCOMPARE(described.errors().first().message, QStringLiteral("studio runs no model it shares with this machine."));
        QVERIFY(described.errors().first().hint.contains("omarchy-agent-local share"));

        network("offline");
        described = await<Description>([&](auto reply) { provider->describe(reply); });
        QCOMPARE(described.errors().first().message, QStringLiteral("studio is offline."));
    }

    void anUnpairedHostSaysHowToPair() {
        Fixture fixture;
        ProviderInstance instance;
        instance.name = QStringLiteral("Studio box");
        instance.address = QStringLiteral("https://studio.tail1234.ts.net");
        auto provider = fixture.provider(instance);
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!described.ok());
        QCOMPARE(described.errors().first().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.errors().first().hint.contains("omarchy-agent-local pair studio"));
    }

    void generateWritesTheImageTheMomentTheJobFinishes() {
        Fixture fixture;
        fixture.runKlein();
        fixture.acceptJobs("job_1");
        const QJsonObject info{{"seed", "123456789012"}, {"width", 1024}, {"height", 768},
                               {"sample_params", QJsonObject{{"sample_steps", 4}}}};
        // After the completed answer the job is gone (410): the image must already be on disk.
        fixture.scriptJob("job_1", {job("queued"), sampling(2, 4, "PREVIEW"), completed("PNGDATA", info)});
        auto provider = fixture.provider();
        QList<JobStatus> stream;
        connect(provider.get(), &Provider::statusChanged, this,
                [&](const QString &, const JobStatus &status) { stream.append(status); });

        const JobRequest request = fixture.request(
            "image.generate", {{"prompt", "a lighthouse"}, {"negative_prompt", "blurry"}, {"width", 1024},
                               {"height", 768}, {"steps", 4}, {"guidance", 1.0}, {"seed", -1}});
        const auto submitted = await<QString>([&](auto reply) { provider->submit(request, reply); });
        QVERIFY2(submitted.ok(), qPrintable(submitted.errors().value(0).message));
        const QString id = submitted.value();

        const QJsonObject body = requestBody(fixture.server);
        QCOMPARE(body.value("prompt").toString(), QStringLiteral("a lighthouse"));
        QCOMPARE(body.value("negative_prompt").toString(), QStringLiteral("blurry"));
        QCOMPARE(body.value("width").toInt(), 1024);
        QCOMPARE(body.value("height").toInt(), 768);
        QCOMPARE(body.value("seed").toInt(), -1);
        QCOMPARE(body.value("sample_params").toObject().value("sample_steps").toInt(), 4);
        QCOMPARE(body.value("sample_params").toObject().value("guidance").toObject().value("txt_cfg").toDouble(), 1.0);
        QCOMPARE(body.value("batch_count").toInt(), 1);
        QCOMPARE(body.value("output_format").toString(), QStringLiteral("png"));
        QCOMPARE(body.value("preview").toString(), QStringLiteral("proj"));
        QVERIFY(!body.contains("init_image"));
        QCOMPARE(fixture.server.requestsTo("POST", "/sdcpp/v1/img_gen").first().headers.value("authorization"),
                 "Bearer " + kKey);

        QTRY_VERIFY(!stream.isEmpty() && stream.last().isTerminal());
        QCOMPARE(stream.last().state, QStringLiteral("done"));
        bool sawSampling = false;
        for (const JobStatus &status : stream)
            if (status.stage == QLatin1String("sampling")) {
                sawSampling = true;
                QCOMPARE(status.state, QStringLiteral("running"));
                QCOMPARE(status.progress, 0.5);
                QCOMPARE(status.preview, QByteArray("PREVIEW"));
            }
        QVERIFY(sawSampling);

        // A provider that kept polling would ask again within one poll interval and get
        // the 410; five intervals pass and it has not.
        QTest::qWait(5 * Fixture::pollMs);
        QCOMPARE(fixture.server.requestsTo("GET", "/sdcpp/v1/jobs/job_1").size(), 3);
        const auto result = await<JobResult>([&](auto reply) { provider->result(id, reply); });
        QVERIFY(result.ok());
        const QJsonObject image = result.value().outputs.value("image").toObject();
        QCOMPARE(image.value("path").toString(), QDir(request.outputDir).filePath("image.png"));
        QCOMPARE(image.value("format").toString(), QStringLiteral("png"));
        QCOMPARE(readAll(image.value("path").toString()), QByteArray("PNGDATA"));
        const QJsonObject used = result.value().used;
        QCOMPARE(used.value("seed").toInteger(), Q_INT64_C(123456789012));
        QCOMPARE(used.value("width").toInt(), 1024);
        QCOMPARE(used.value("height").toInt(), 768);
        QCOMPARE(used.value("steps").toInt(), 4);

        // The result is handed over once; the provider then forgets the job, so a
        // long-lived process does not keep every finished job.
        QVERIFY(!await<JobStatus>([&](auto reply) { provider->status(id, reply); }).ok());
        QVERIFY(!await<JobResult>([&](auto reply) { provider->result(id, reply); }).ok());
        QCOMPARE(used.value("engine").toString(), QStringLiteral("sdcpp"));
        QCOMPARE(used.value("model").toObject().value("name").toString(), kKlein);
    }

    void startAndReferenceImagesTravelAsBase64() {
        Fixture fixture;
        fixture.runKlein();
        fixture.acceptJobs("job_1");
        fixture.scriptJob("job_1", {});
        auto provider = fixture.provider();
        const QString start = fixture.writeInput("start.png", "START");
        const QString a = fixture.writeInput("a.png", "AAA");
        const QString b = fixture.writeInput("b.png", "BBB");

        auto generate = fixture.request("image.generate", {{"prompt", "redraw"}, {"strength", 0.6}},
                                        {{"image", QJsonObject{{"path", start}}}});
        QVERIFY(await<QString>([&](auto reply) { provider->submit(generate, reply); }).ok());
        QJsonObject body = requestBody(fixture.server);
        QCOMPARE(QByteArray::fromBase64(body.value("init_image").toString().toLatin1()), QByteArray("START"));
        QCOMPARE(body.value("strength").toDouble(), 0.6);

        auto edit = fixture.request("image.edit", {{"prompt", "make it night"}},
                                    {{"images", QJsonArray{QJsonObject{{"path", a}}, QJsonObject{{"path", b}}}}});
        edit.model = QJsonObject{{"resource", "fugue/image-model"}, {"name", kKlein}};
        QVERIFY(await<QString>([&](auto reply) { provider->submit(edit, reply); }).ok());
        body = requestBody(fixture.server);
        const QJsonArray refs = body.value("ref_images").toArray();
        QCOMPARE(refs.size(), 2);
        QCOMPARE(QByteArray::fromBase64(refs.at(0).toString().toLatin1()), QByteArray("AAA"));
        QCOMPARE(QByteArray::fromBase64(refs.at(1).toString().toLatin1()), QByteArray("BBB"));
        QVERIFY(!body.contains("init_image"));
    }

    void aMissingInputFileIsFileNotFound() {
        Fixture fixture;
        fixture.runKlein();
        auto provider = fixture.provider();
        auto edit = fixture.request("image.edit", {{"prompt", "x"}},
                                    {{"images", QJsonArray{QJsonObject{{"path", fixture.home.filePath("nope.png")}}}}});
        const auto submitted = await<QString>([&](auto reply) { provider->submit(edit, reply); });
        QVERIFY(!submitted.ok());
        QCOMPARE(submitted.errors().first().code, QStringLiteral("file_not_found"));
        QVERIFY(fixture.server.requestsTo("POST", "/sdcpp/v1/img_gen").isEmpty());
    }

    void aRequestedModelGoesToItsOwnServer() {
        Fixture fixture;
        FakeHttpServer other;
        QVERIFY(other.listen());
        other.on("GET", "/sdcpp/v1/capabilities",
                 [](const HttpRequest &) { return HttpResponse{200, json(capabilities())}; });
        other.on("POST", "/sdcpp/v1/img_gen", [](const HttpRequest &) {
            return HttpResponse{202, json(QJsonObject{{"id", "job_9"}, {"status", "queued"}})};
        });
        QJsonObject klein = row(kKlein, "image", "active", fixture.endpoint());
        klein.insert("edit", true);
        fixture.writeStatus(QJsonObject{
            {kKlein, klein},
            {kZImage, row(kZImage, "image", "active", QString::fromLatin1(other.origin()) + "/v1")}});
        auto provider = fixture.provider();
        auto request = fixture.request("image.generate", {{"prompt", "x"}});
        request.model = QJsonObject{{"resource", "fugue/image-model"}, {"name", kZImage}};
        const auto submitted = await<QString>([&](auto reply) { provider->submit(request, reply); });
        QVERIFY(submitted.ok());
        QCOMPARE(other.requestsTo("POST", "/sdcpp/v1/img_gen").size(), 1);
        QVERIFY(fixture.server.requestsTo("POST", "/sdcpp/v1/img_gen").isEmpty());
    }

    void aRequestedModelThatIsStoppedIsModelNotRunning() {
        Fixture fixture;
        fixture.writeStatus(QJsonObject{{kZImage, row(kZImage, "image", "inactive", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("image.generate", {{"prompt", "x"}});
        request.model = QJsonObject{{"resource", "fugue/image-model"}, {"name", kZImage}};
        const auto submitted = await<QString>([&](auto reply) { provider->submit(request, reply); });
        QVERIFY(!submitted.ok());
        QCOMPARE(submitted.errors().first().code, QStringLiteral("model_not_running"));
        QVERIFY(submitted.errors().first().hint.contains("omarchy-agent-local start " + kZImage));
    }

    void aRefusedSubmissionSaysWhy() {
        Fixture fixture;
        fixture.runKlein();
        fixture.server.on("POST", "/sdcpp/v1/img_gen", [](const HttpRequest &) {
            return HttpResponse{400, "{\"error\":\"width must be a multiple of 16\"}"};
        });
        auto provider = fixture.provider();
        auto request = fixture.request("image.generate", {{"prompt", "x"}, {"width", 1000}});
        const auto submitted = await<QString>([&](auto reply) { provider->submit(request, reply); });
        QVERIFY(!submitted.ok());
        QCOMPARE(submitted.errors().first().code, QStringLiteral("node_failed"));
        QVERIFY(submitted.errors().first().message.contains("multiple of 16"));
    }

    void cancelKeepsFollowingUntilTheServerSaysCancelled() {
        Fixture fixture;
        fixture.runKlein();
        fixture.acceptJobs("job_1");
        // Cooperative cancel (api.md "POST /sdcpp/v1/jobs/{id}/cancel"): still generating once
        // after the request, then cancelled.
        auto cancelled = std::make_shared<bool>(false);
        auto afterCancel = std::make_shared<int>(0);
        fixture.server.on("GET", "/sdcpp/v1/jobs/job_1", [cancelled, afterCancel](const HttpRequest &) {
            if (!*cancelled)
                return HttpResponse{200, json(sampling(1, 4))};
            return HttpResponse{200, json(++*afterCancel < 2 ? sampling(2, 4)
                                                             : job("cancelled", {{"error", QJsonObject{{"code", "cancelled"}, {"message", "job cancelled by client"}}}}))};
        });
        fixture.server.on("POST", "/sdcpp/v1/jobs/job_1/cancel", [cancelled](const HttpRequest &) {
            *cancelled = true;
            QJsonObject answer = sampling(1, 4);
            answer.insert("cancel_requested", true);
            return HttpResponse{200, json(answer)};
        });
        auto provider = fixture.provider();
        QList<JobStatus> stream;
        connect(provider.get(), &Provider::statusChanged, this,
                [&](const QString &, const JobStatus &status) { stream.append(status); });
        auto request = fixture.request("image.generate", {{"prompt", "x"}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        QTRY_VERIFY(!stream.isEmpty() && stream.last().state == QLatin1String("running"));

        const auto asked = await<QString>([&](auto reply) { provider->cancel(id, reply); });
        QVERIFY(asked.ok());
        QCOMPARE(asked.value(), QStringLiteral("running"));
        QTRY_COMPARE(stream.last().state, QStringLiteral("cancelled"));
        QVERIFY(*afterCancel >= 2);
        QVERIFY(!await<JobResult>([&](auto reply) { provider->result(id, reply); }).ok());
    }

    void aFailedJobIsAnErrorAndOutOfMemoryIsItsOwnCode() {
        Fixture fixture;
        fixture.runKlein();
        fixture.acceptJobs("job_1");
        fixture.scriptJob("job_1", {job("failed", {{"error", QJsonObject{{"code", "generation_failed"},
                                                                         {"message", "CUDA error: out of memory"}}}})});
        auto provider = fixture.provider();
        auto request = fixture.request("image.generate", {{"prompt", "x"}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        JobStatus status;
        QTRY_VERIFY((status = await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value()).isTerminal());
        QCOMPARE(status.state, QStringLiteral("error"));
        QCOMPARE(status.error.code, QStringLiteral("out_of_memory"));
        QVERIFY(status.error.message.contains("out of memory"));
    }

    // A job with no result to take (failed or cancelled) is kept as long as the server
    // keeps its own finished jobs, 600 seconds (8.6), then forgotten.
    void aFinishedJobNobodyAsksAboutIsForgottenAfterTenMinutes() {
        Fixture fixture;
        fixture.runKlein();
        fixture.acceptJobs("job_1");
        fixture.scriptJob("job_1", {job("failed", {{"error", QJsonObject{{"code", "generation_failed"}, {"message", "no"}}}})});
        auto provider = fixture.provider();
        qint64 now = 1000;
        provider->setClock([&now] { return now; });
        const QString id = await<QString>([&](auto reply) { provider->submit(fixture.request("image.generate", {{"prompt", "x"}}), reply); }).value();
        QTRY_VERIFY(await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value().isTerminal());

        now += 599 * 1000;
        QVERIFY(await<JobStatus>([&](auto reply) { provider->status(id, reply); }).ok());
        now += 2 * 1000;
        QVERIFY(!await<JobStatus>([&](auto reply) { provider->status(id, reply); }).ok());
    }

    void aServerThatDiesDuringTheJobIsUnreachable() {
        Fixture fixture;
        fixture.runKlein();
        fixture.acceptJobs("job_1");
        auto provider = fixture.provider();
        auto request = fixture.request("image.generate", {{"prompt", "x"}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        fixture.server.close();
        JobStatus status;
        QTRY_VERIFY((status = await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value()).isTerminal());
        QCOMPARE(status.state, QStringLiteral("error"));
        QCOMPARE(status.error.code, QStringLiteral("provider_unreachable"));
    }

    void aJobTheServerDroppedIsAnError() {
        Fixture fixture;
        fixture.runKlein();
        fixture.acceptJobs("job_1");
        fixture.scriptJob("job_1", {});
        auto provider = fixture.provider();
        auto request = fixture.request("image.generate", {{"prompt", "x"}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        JobStatus status;
        QTRY_VERIFY((status = await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value()).isTerminal());
        QCOMPARE(status.state, QStringLiteral("error"));
        QCOMPARE(status.error.code, QStringLiteral("node_failed"));
    }

    // Text models (kind text) serve text.generate over their OpenAI-compatible API
    // (omarchy-local-ai runtimes' api = "openai").
    void describeOffersTextModelsForTextGenerate() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.writeStatus(QJsonObject{
            {kQwen, row(kQwen, "text", "active", fixture.endpoint())},
            {"nemotron-3.5-bolt-tensorfold", row("nemotron-3.5-bolt-tensorfold", "text", "inactive", fixture.endpoint())},
        });
        auto provider = fixture.provider();

        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(described.ok());
        QVERIFY(!described.value().offer("image.generate"));
        QCOMPARE(described.value().offer("text.generate")->models.size(), 2);
        const OfferedModel *ready = findModel(described.value(), "text.generate", kQwen);
        QCOMPARE(ready->status, ModelStatus::Ready);
        QCOMPARE(ready->handle, (QJsonObject{{"resource", "fugue/text-model"}, {"name", kQwen}}));
        // A local row does not say what it takes (agent_local.py model_record has no input).
        QVERIFY(!ready->properties.contains("input"));
        const OfferedModel *stopped = findModel(described.value(), "text.generate", "nemotron-3.5-bolt-tensorfold");
        QCOMPARE(stopped->status, ModelStatus::Unavailable);
        QVERIFY(stopped->hint.contains("omarchy-agent-local start nemotron-3.5-bolt-tensorfold"));
        QCOMPARE(fixture.server.requestsTo("GET", "/v1/models").size(), 1);
    }

    void aTextServerStillLoadingIsLoading() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &) {
            return HttpResponse{503, "{\"error\":{\"message\":\"Loading model\",\"code\":503}}"};
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QCOMPARE(findModel(described.value(), "text.generate", kQwen)->status, ModelStatus::Loading);
    }

    void aTextJobStreamsItsAnswer() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &) {
            return HttpResponse{200, streamed({"A red ", "fox at dusk"}, "The user wants a fox."), "text/event-stream"};
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("text.generate", {{"system", "Be brief."}, {"prompt", "fox"}, {"seed", 7},
                                                         {"max_tokens", 200}, {"temperature", 0.5}});

        const auto submitted = await<QString>([&](auto reply) { provider->submit(request, reply); });
        QVERIFY2(submitted.ok(), qPrintable(submitted.ok() ? QString() : submitted.error().message));
        JobStatus status;
        QTRY_VERIFY((status = await<JobStatus>([&](auto reply) { provider->status(submitted.value(), reply); }).value()).isTerminal());
        QCOMPARE(status.state, QStringLiteral("done"));
        const auto result = await<JobResult>([&](auto reply) { provider->result(submitted.value(), reply); });
        QVERIFY(result.ok());
        QCOMPARE(result->outputs.value("text").toString(), QStringLiteral("A red fox at dusk"));
        QCOMPARE(result->used.value("model").toObject(), (QJsonObject{{"resource", "fugue/text-model"}, {"name", kQwen}}));
        QCOMPARE(result->used.value("seed").toInt(), 7);

        const auto posts = fixture.server.requestsTo("POST", "/v1/chat/completions");
        QCOMPARE(posts.size(), 1);
        QCOMPARE(posts.first().headers.value("authorization"), "Bearer " + kKey);
        const QJsonObject body = chatBody(fixture.server);
        QCOMPARE(body.value("model").toString(), kQwen); // the served name is the recipe id
        QCOMPARE(body.value("stream").toBool(), true);
        QCOMPARE(body.value("seed").toInt(), 7);
        QCOMPARE(body.value("max_tokens").toInt(), 200);
        QCOMPARE(body.value("temperature").toDouble(), 0.5);
        QCOMPARE(body.value("messages").toArray(),
                 (QJsonArray{QJsonObject{{"role", "system"}, {"content", "Be brief."}},
                             QJsonObject{{"role", "user"}, {"content", "fox"}}}));
    }

    void aTextJobWithNoSystemPromptSendsOnlyTheUsers() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &) {
            return HttpResponse{200, streamed({"ok"}), "text/event-stream"};
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("text.generate", {{"prompt", "fox"}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        QTRY_VERIFY(await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value().isTerminal());
        const QJsonObject body = chatBody(fixture.server);
        QCOMPARE(body.value("messages").toArray(), (QJsonArray{QJsonObject{{"role", "user"}, {"content", "fox"}}}));
        // Unset: the server's own.
        QVERIFY(!body.contains("seed"));
        QVERIFY(!body.contains("max_tokens"));
        QVERIFY(!body.contains("temperature"));
    }

    // Thinking goes out as the chat template's switch (Qwen3 templates; llama.cpp, vLLM and
    // tensorfold take it). Not OpenAI's reasoning_effort: a strict server refuses "none".
    void thinkingCanBeTurnedOff() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &) {
            return HttpResponse{200, streamed({"ok"}), "text/event-stream"};
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        for (const bool thinking : {false, true}) {
            auto request = fixture.request("text.generate", {{"prompt", "fox"}, {"thinking", thinking}});
            const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
            QTRY_VERIFY(await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value().isTerminal());
            const QJsonObject body = chatBody(fixture.server);
            QCOMPARE(body.value("chat_template_kwargs").toObject(), (QJsonObject{{"enable_thinking", thinking}}));
            QVERIFY(!body.contains("reasoning_effort"));
        }
        // Unsaid: the model's own way.
        QVERIFY(!QJsonDocument::fromJson(fixture.server.requestsTo("POST", "/v1/chat/completions").first().body)
                     .object()
                     .contains("max_tokens"));
    }

    void aTextJobSendsItsImageAsADataUrl() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &) {
            return HttpResponse{200, streamed({"a fox"}), "text/event-stream"};
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const QByteArray png = "\x89PNG fake bytes";
        const QString path = fixture.writeInput("photo.png", png);
        auto request = fixture.request("text.generate", {{"prompt", "Describe it."}}, {{"image", QJsonObject{{"path", path}}}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        QTRY_VERIFY(await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value().isTerminal());
        const QJsonArray messages = chatBody(fixture.server).value("messages").toArray();
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.first().toObject().value("content").toArray(),
                 (QJsonArray{QJsonObject{{"type", "text"}, {"text", "Describe it."}},
                             QJsonObject{{"type", "image_url"},
                                         {"image_url", QJsonObject{{"url", "data:image/png;base64," +
                                                                               QString::fromLatin1(png.toBase64())}}}}}));
    }

    void aServerThatDoesNotStreamStillAnswers() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &) {
            return HttpResponse{200, json(QJsonObject{{"choices", QJsonArray{QJsonObject{
                                                                       {"index", 0},
                                                                       {"message", QJsonObject{{"role", "assistant"}, {"content", "plain answer"}}},
                                                                       {"finish_reason", "stop"}}}}})};
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("text.generate", {{"prompt", "fox"}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        QTRY_VERIFY(await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value().isTerminal());
        const auto result = await<JobResult>([&](auto reply) { provider->result(id, reply); });
        QVERIFY(result.ok());
        QCOMPARE(result->outputs.value("text").toString(), QStringLiteral("plain answer"));
    }

    void aRefusedTextJobSaysWhy() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &) {
            return HttpResponse{400, "{\"error\":{\"message\":\"the request exceeds the context size\",\"type\":\"exceed_context_size_error\"}}"};
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("text.generate", {{"prompt", "fox"}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        JobStatus status;
        QTRY_VERIFY((status = await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value()).isTerminal());
        QCOMPARE(status.state, QStringLiteral("error"));
        QCOMPARE(status.error.code, QStringLiteral("node_failed"));
        QVERIFY2(status.error.message.contains("exceeds the context size"), qPrintable(status.error.message));
    }

    void cancellingATextJobStopsIt() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &) {
            HttpResponse working;
            working.hold = true;
            return working;
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("text.generate", {{"prompt", "fox"}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        QTRY_COMPARE(fixture.server.requestsTo("POST", "/v1/chat/completions").size(), 1);
        QCOMPARE(await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value().state, QStringLiteral("running"));
        const auto cancelled = await<QString>([&](auto reply) { provider->cancel(id, reply); });
        QVERIFY(cancelled.ok());
        QCOMPARE(cancelled.value(), QStringLiteral("cancelled"));
        QCOMPARE(await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value().state, QStringLiteral("cancelled"));
    }

    // The node says what the picture is (MediaHandle::format, from its bytes); the name may not.
    void aTextJobsImageTypeIsTheOneTheNodeSays() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &) {
            return HttpResponse{200, streamed({"a fox"}), "text/event-stream"};
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const QString path = fixture.writeInput("photo.jfif", "\xff\xd8 jpeg bytes");
        auto request = fixture.request("text.generate", {{"prompt", "Describe it."}},
                                       {{"image", QJsonObject{{"path", path}, {"format", "jpeg"}}}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        QTRY_VERIFY(await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value().isTerminal());
        const QString url = chatBody(fixture.server).value("messages").toArray().first().toObject().value("content")
                                .toArray().at(1).toObject().value("image_url").toObject().value("url").toString();
        QVERIFY2(url.startsWith("data:image/jpeg;base64,"), qPrintable(url.left(40)));
    }

    // A stream that ends without its finish ([DONE] or a finish_reason) was cut off: the server
    // died or the proxy dropped it. Half an answer is not an answer.
    void aStreamCutOffMidwayIsAnError() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &) {
            QByteArray cut = streamed({"A red ", "fox"});
            cut.truncate(cut.indexOf("fox") + 10); // inside the second piece's chunk, no finish
            cut.truncate(cut.lastIndexOf("data:"));
            return HttpResponse{200, cut, "text/event-stream"};
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("text.generate", {{"prompt", "fox"}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        JobStatus status;
        QTRY_VERIFY((status = await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value()).isTerminal());
        QCOMPARE(status.state, QStringLiteral("error"));
        QCOMPARE(status.error.code, QStringLiteral("provider_unreachable"));
        QVERIFY2(status.error.message.contains("stopped in the middle"), qPrintable(status.error.message));
    }

    // The last line of a stream may have no line end.
    void theLastChunkCountsWithoutALineEnd() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &) {
            QByteArray body = streamed({"A red fox"});
            body.chop(QByteArray("data: [DONE]\n\n").size());
            return HttpResponse{200, body.trimmed(), "text/event-stream"}; // ends on the finish chunk
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("text.generate", {{"prompt", "fox"}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        JobStatus status;
        QTRY_VERIFY((status = await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value()).isTerminal());
        QCOMPARE(status.state, QStringLiteral("done"));
        QCOMPARE(await<JobResult>([&](auto reply) { provider->result(id, reply); })->outputs.value("text").toString(),
                 QStringLiteral("A red fox"));
    }

    void aTextModelOutOfMemoryIsNotToldToMakeASmallerImage() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &request) { return modelsList(request, kQwen); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &) {
            return HttpResponse{500, "{\"error\":{\"message\":\"CUDA error: out of memory\"}}"};
        });
        fixture.writeStatus(QJsonObject{{kQwen, row(kQwen, "text", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("text.generate", {{"prompt", "fox"}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        JobStatus status;
        QTRY_VERIFY((status = await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value()).isTerminal());
        QCOMPARE(status.error.code, QStringLiteral("out_of_memory"));
        QVERIFY2(!status.error.hint.contains("image"), qPrintable(status.error.hint));
    }

    // --- video (video.generate, sd-server vid_gen) and music (audio.generate, ace-server) ---

    void videoAndMusicRowsAreOfferedAsTheirOwnCapabilities() {
        Fixture fixture;
        fixture.server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) {
            QJsonObject caps = capabilities();
            caps.insert("supported_modes", QJsonArray{"vid_gen"});
            caps.insert("defaults_by_mode",
                        QJsonObject{{"vid_gen", QJsonObject{{"width", 832}, {"height", 480}, {"fps", 16},
                                                            {"sample_params", QJsonObject{{"sample_steps", 4}}}}}});
            return HttpResponse{200, json(caps)};
        });
        fixture.server.on("GET", "/health", [](const HttpRequest &) { return HttpResponse{200, "{\"status\":\"ok\"}"}; });
        fixture.writeStatus(QJsonObject{{kVideoModel, row(kVideoModel, "video", "active", fixture.endpoint())},
                                        {kMusicModel, row(kMusicModel, "music", "active", fixture.endpoint())},
                                        {kKlein, row(kKlein, "image", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(described.ok());

        const OfferedModel *video = findModel(described.value(), "video.generate", kVideoModel);
        QVERIFY(video);
        QCOMPARE(video->handle.value("resource").toString(), QStringLiteral("fugue/video-model"));
        QCOMPARE(video->status, ModelStatus::Ready);
        QCOMPARE(video->defaults.value("width").toInt(), 832);
        QCOMPARE(video->defaults.value("fps").toInt(), 16);
        QCOMPARE(video->defaults.value("steps").toInt(), 4);

        const OfferedModel *music = findModel(described.value(), "audio.generate", kMusicModel);
        QVERIFY(music);
        QCOMPARE(music->handle.value("resource").toString(), QStringLiteral("fugue/music-model"));
        QCOMPARE(music->status, ModelStatus::Ready);
        QCOMPARE(music->limits.value("max_duration").toInt(), 600);

        // Neither is an image model, and the image model is neither.
        QVERIFY(!findModel(described.value(), "image.generate", kVideoModel));
        QVERIFY(!findModel(described.value(), "audio.generate", kVideoModel));
    }

    // What MiniMax H3's server reported on the DGX (identifiers and paths removed): only vid_gen,
    // though the model family has image variants. A model is offered for the modes its server
    // lists, so this one stays a video model.
    void aVideoOnlyServerOffersNoImageCapability() {
        Fixture fixture;
        fixture.server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) {
            QJsonObject caps = capabilities();
            caps.insert("supported_modes", QJsonArray{"vid_gen"});
            caps.insert("current_mode", "vid_gen");
            caps.insert("features_by_mode",
                        QJsonObject{{"vid_gen", QJsonObject{{"init_image", true}, {"end_image", true},
                                                            {"ref_images", true}}}});
            caps.insert("defaults_by_mode", QJsonObject{{"vid_gen", QJsonObject{{"sample_params", QJsonObject{{"sample_steps", 4}}}}}});
            return HttpResponse{200, json(caps)};
        });
        fixture.writeStatus(QJsonObject{{kVideoModel, row(kVideoModel, "video", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(described.ok());
        QVERIFY(findModel(described.value(), "video.generate", kVideoModel));
        QVERIFY(!findModel(described.value(), "image.generate", kVideoModel));
        QVERIFY(!findModel(described.value(), "image.edit", kVideoModel));
    }

    // A server that serves img_gen and vid_gen offers its model for both, whichever kind its row
    // has; reference images (features_by_mode.img_gen.ref_images) make it an edit model.
    void aModelIsOfferedForEveryModeItsServerServes() {
        Fixture fixture;
        fixture.server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) {
            QJsonObject caps = capabilities();
            caps.insert("supported_modes", QJsonArray{"img_gen", "vid_gen"});
            caps.insert("features_by_mode", QJsonObject{{"img_gen", QJsonObject{{"init_image", true}, {"ref_images", true}}},
                                                        {"vid_gen", QJsonObject{{"init_image", true}}}});
            caps.insert("defaults_by_mode",
                        QJsonObject{{"img_gen", QJsonObject{{"width", 1024}, {"height", 1024}}},
                                    {"vid_gen", QJsonObject{{"width", 832}, {"height", 480}, {"fps", 24}}}});
            return HttpResponse{200, json(caps)};
        });
        fixture.acceptJobs("img_1");
        fixture.writeStatus(QJsonObject{{kVideoModel, row(kVideoModel, "video", "active", fixture.endpoint())},
                                        {kKlein, row(kKlein, "image", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(described.ok());

        const OfferedModel *video = findModel(described.value(), "video.generate", kVideoModel);
        QVERIFY(video);
        QCOMPARE(video->handle.value("resource").toString(), QStringLiteral("fugue/video-model"));
        QCOMPARE(video->defaults.value("width").toInt(), 832);
        const OfferedModel *image = findModel(described.value(), "image.generate", kVideoModel);
        QVERIFY(image);
        QCOMPARE(image->handle.value("resource").toString(), QStringLiteral("fugue/image-model"));
        QCOMPARE(image->defaults.value("width").toInt(), 1024);
        QVERIFY(findModel(described.value(), "image.edit", kVideoModel));
        // The image row that serves vid_gen is a video model too; it is not an edit model (its
        // row says edit is false and it has no ref_images of its own: the row decides).
        QVERIFY(findModel(described.value(), "video.generate", kKlein));
        QVERIFY(findModel(described.value(), "image.generate", kKlein));
        QVERIFY(!findModel(described.value(), "image.edit", kKlein));

        // A job for the image capability goes to img_gen with that model.
        JobRequest request = fixture.request("image.generate", {{"prompt", "a cat"}});
        request.model = image->handle;
        const auto submitted = await<QString>([&](auto reply) { provider->submit(request, reply); });
        QVERIFY2(submitted.ok(), "submit failed");
        QCOMPARE(fixture.server.requestsTo("POST", "/sdcpp/v1/img_gen").size(), 1);
        QCOMPARE(fixture.server.requestsTo("POST", "/sdcpp/v1/vid_gen").size(), 0);
    }

    void aVideoJobSendsEveryLoraAndTheFirstFrameAndKeepsTheClipAsAFile() {
        Fixture fixture;
        fixture.server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) {
            QJsonObject caps = capabilities();
            caps.insert("loras", QJsonArray{QJsonObject{{"name", "turbo"}, {"path", "/models/loras/turbo.safetensors"}}});
            return HttpResponse{200, json(caps)};
        });
        const QByteArray webm = "\x1a\x45\xdf\xa3 pretend webm";
        fixture.server.on("POST", "/sdcpp/v1/vid_gen", [](const HttpRequest &) {
            return HttpResponse{202, json(QJsonObject{{"id", "vid_1"}, {"kind", "vid_gen"}, {"status", "queued"}})};
        });
        fixture.server.on("GET", "/sdcpp/v1/jobs/vid_1", [webm](const HttpRequest &) {
            return HttpResponse{200, json(job("completed",
                                              {{"result", QJsonObject{{"output_format", "webm"}, {"mime_type", "video/webm"},
                                                                      {"fps", 24}, {"frame_count", 53},
                                                                      {"b64_json", QString::fromLatin1(webm.toBase64())}}}}))};
        });
        fixture.writeStatus(QJsonObject{{kVideoModel, row(kVideoModel, "video", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        const QString frame = fixture.writeInput("frame.png", "first frame bytes");
        auto request = fixture.request("video.generate",
                                       {{"prompt", "a fox runs"}, {"negative_prompt", "blur"}, {"width", 864}, {"height", 480},
                                        {"video_frames", 48}, {"fps", 24}, {"steps", 4}, {"guidance", 1.5}, {"seed", 9}},
                                       {{"image", QJsonObject{{"path", frame}}}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        JobStatus status;
        QTRY_VERIFY((status = await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value()).isTerminal());
        QCOMPARE(status.state, QStringLiteral("done"));

        const QJsonObject body = QJsonDocument::fromJson(fixture.server.requestsTo("POST", "/sdcpp/v1/vid_gen").first().body).object();
        QCOMPARE(body.value("prompt").toString(), QStringLiteral("a fox runs"));
        QCOMPARE(body.value("negative_prompt").toString(), QStringLiteral("blur"));
        QCOMPARE(body.value("width").toInt(), 864);
        QCOMPARE(body.value("video_frames").toInt(), 48);
        QCOMPARE(body.value("fps").toInt(), 24);
        QCOMPARE(body.value("seed").toInt(), 9);
        QCOMPARE(body.value("output_format").toString(), QStringLiteral("webm"));
        QCOMPARE(body.value("vae_tiling_params").toObject().value("enabled").toBool(), true);
        QCOMPARE(body.value("sample_params").toObject().value("sample_steps").toInt(), 4);
        QCOMPARE(body.value("sample_params").toObject().value("guidance").toObject().value("txt_cfg").toDouble(), 1.5);
        QCOMPARE(body.value("init_image").toString(), QString::fromLatin1(QByteArray("first frame bytes").toBase64()));
        // The server applies no LoRA by itself, so each one it lists goes with the job (8.7).
        QCOMPARE(body.value("lora").toArray(),
                 (QJsonArray{QJsonObject{{"path", "/models/loras/turbo.safetensors"}, {"multiplier", 1.0}}}));

        const auto result = await<JobResult>([&](auto reply) { provider->result(id, reply); });
        QVERIFY(result.ok());
        const QJsonObject video = result->outputs.value("video").toObject();
        QCOMPARE(video.value("format").toString(), QStringLiteral("webm"));
        QCOMPARE(readAll(video.value("path").toString()), webm);
        QCOMPARE(result->used.value("seed").toInt(), 9);
        QCOMPARE(result->used.value("frames").toInt(), 53);
        QCOMPARE(result->used.value("engine").toString(), QStringLiteral("sdcpp"));
    }

    void aVideoJobWithoutFirstFrameOrLorasSendsNeither() {
        Fixture fixture;
        fixture.server.on("POST", "/sdcpp/v1/vid_gen", [](const HttpRequest &) {
            return HttpResponse{400, json(QJsonObject{{"error", "this model needs a first frame"}})};
        });
        fixture.writeStatus(QJsonObject{{kVideoModel, row(kVideoModel, "video", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("video.generate", {{"prompt", "a fox runs"}});
        const auto refused = await<QString>([&](auto reply) { provider->submit(request, reply); });
        QVERIFY(!refused.ok());
        QVERIFY2(refused.error().message.contains("needs a first frame"), qPrintable(refused.error().message));
        const QJsonObject body = QJsonDocument::fromJson(fixture.server.requestsTo("POST", "/sdcpp/v1/vid_gen").first().body).object();
        QVERIFY(!body.contains("init_image"));
        QVERIFY(!body.contains("lora"));
    }

    // ace-server's two jobs in turn (Fantasi's makeSong): /lm writes the song, /synth renders it
    // as it was written, and the first part of the multipart answer is the song.
    void aSongIsWrittenThenRenderedAndItsFirstPartIsTheAudio() {
        Fixture fixture;
        fixture.server.on("GET", "/health", [](const HttpRequest &) { return HttpResponse{200, "{\"status\":\"ok\"}"}; });
        fixture.server.on("POST", "/lm", [](const HttpRequest &) { return HttpResponse{200, "{\"id\":\"1\"}"}; });
        fixture.server.on("GET", "/job?id=1", [](const HttpRequest &) { return HttpResponse{200, "{\"status\":\"done\"}"}; });
        const QJsonObject written{{"caption", "a calm piano piece"}, {"lyrics", "[Verse]\nla la"}, {"bpm", 80}, {"seed", 4242},
                                  {"audio_codes", "1,2,3"}};
        fixture.server.on("GET", "/job?id=1&result=1", [written](const HttpRequest &) {
            return HttpResponse{200, QJsonDocument(QJsonArray{written}).toJson(QJsonDocument::Compact)};
        });
        fixture.server.on("POST", "/synth", [](const HttpRequest &) { return HttpResponse{200, "{\"id\":\"2\"}"}; });
        fixture.server.on("GET", "/job?id=2", [](const HttpRequest &) { return HttpResponse{200, "{\"status\":\"done\"}"}; });
        const QByteArray mp3 = "ID3 pretend mp3 \r\n bytes";
        fixture.server.on("GET", "/job?id=2&result=1", [mp3](const HttpRequest &) {
            return HttpResponse{200,
                                "--B7\r\nContent-Type: audio/mpeg\r\n\r\n" + mp3 + "\r\n--B7\r\nContent-Type: application/octet-stream\r\n\r\nLATENT\r\n--B7--\r\n",
                                "multipart/mixed; boundary=B7"};
        });
        fixture.writeStatus(QJsonObject{{kMusicModel, row(kMusicModel, "music", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("audio.generate", {{"prompt", "a calm piano piece"}, {"lyrics", ""}, {"duration", 40},
                                                          {"language", "en"}, {"seed", 4242}});
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        JobStatus status;
        QTRY_VERIFY((status = await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value()).isTerminal());
        QCOMPARE(status.state, QStringLiteral("done"));

        const QJsonObject lm = QJsonDocument::fromJson(fixture.server.requestsTo("POST", "/lm").first().body).object();
        QCOMPARE(lm.value("caption").toString(), QStringLiteral("a calm piano piece"));
        QCOMPARE(lm.value("lyrics").toString(), QString());
        QCOMPARE(lm.value("duration").toInt(), 40);
        QCOMPARE(lm.value("vocal_language").toString(), QStringLiteral("en"));
        QCOMPARE(lm.value("seed").toInt(), 4242);
        QJsonObject sent = QJsonDocument::fromJson(fixture.server.requestsTo("POST", "/synth").first().body).object();
        QCOMPARE(sent.take("output_format").toString(), QStringLiteral("mp3"));
        QCOMPARE(sent, written);

        const auto result = await<JobResult>([&](auto reply) { provider->result(id, reply); });
        QVERIFY(result.ok());
        QCOMPARE(readAll(result->outputs.value("audio").toObject().value("path").toString()), mp3);
        QCOMPARE(result->outputs.value("lyrics").toString(), QStringLiteral("[Verse]\nla la"));
        QCOMPARE(result->used.value("seed").toInt(), 4242);
    }

    void aSongThatFailsAndACancelledSongSayWhere() {
        Fixture fixture;
        fixture.server.on("GET", "/health", [](const HttpRequest &) { return HttpResponse{200, "{}"}; });
        fixture.server.on("POST", "/lm", [](const HttpRequest &) { return HttpResponse{200, "{\"id\":\"1\"}"}; });
        auto state = std::make_shared<QString>("running");
        fixture.server.on("GET", "/job?id=1", [state](const HttpRequest &) {
            return HttpResponse{200, json(QJsonObject{{"status", *state}})};
        });
        fixture.server.on("POST", "/job?id=1&cancel=1", [state](const HttpRequest &) {
            *state = "cancelled";
            return HttpResponse{200, "{\"status\":\"cancelled\"}"};
        });
        fixture.writeStatus(QJsonObject{{kMusicModel, row(kMusicModel, "music", "active", fixture.endpoint())}});
        auto provider = fixture.provider();
        auto request = fixture.request("audio.generate", {{"prompt", "x"}, {"lyrics", ""}, {"duration", 30}});
        QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        QTRY_COMPARE(await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value().stage, QStringLiteral("writing"));
        QCOMPARE(await<QString>([&](auto reply) { provider->cancel(id, reply); }).value(), QStringLiteral("cancelled"));
        QCOMPARE(fixture.server.requestsTo("POST", "/job?id=1&cancel=1").size(), 1);

        *state = "failed";
        id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        JobStatus status;
        QTRY_VERIFY((status = await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value()).isTerminal());
        QCOMPARE(status.state, QStringLiteral("error"));
        QCOMPARE(status.error.code, QStringLiteral("node_failed"));
    }
};

QTEST_GUILESS_MAIN(TestOmarchyProvider)
#include "test_omarchy_provider.moc"
