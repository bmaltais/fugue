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
        QVERIFY(!described.value().offer("text.generate"));

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
};

QTEST_GUILESS_MAIN(TestOmarchyProvider)
#include "test_omarchy_provider.moc"
