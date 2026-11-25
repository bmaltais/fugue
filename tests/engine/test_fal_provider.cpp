// The `fal` provider (spec 8.7, #407) against a fake fal queue, a fake storage and a fake file server on the loopback.
// Every key here is made up. Nothing reaches fal.ai.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/fal_provider.h"
#include "engine/providers.h"
#include "fake_http.h"
#include "../test_shard.h"
#include "provider_support.h"

using namespace fugue;
using fugue::test::await;
using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

const QByteArray kKey = "fal-test-key-0123456789abcdef:secretsecretsecret";

// A 1x1 PNG.
const QByteArray kPng = QByteArray::fromBase64(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");

QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject parse(const QByteArray &bytes) { return QJsonDocument::fromJson(bytes).object(); }

// The queue and storage (the API host) and the file server (the CDN: it must never see the key).
class Fixture {
public:
    Fixture() {
        api.listen();
        cdn.listen();
        // Storage: initiate, then the picture is PUT to the CDN.
        api.on("POST", "/storage/upload/initiate?storage_type=fal-cdn-v3", [this](const HttpRequest &request) {
            if (!keyed(request))
                return refused();
            const QString name = parse(request.body).value("file_name").toString();
            return HttpResponse{200, json({{"file_url", cdnUrl("/files/in-" + name)}, {"upload_url", cdnUrl("/upload/" + name)}})};
        });
        for (const char *name : {"image.png", "mask.png"})
            cdn.on("PUT", QByteArray("/upload/") + name, [](const HttpRequest &) { return HttpResponse{200, "{}"}; });
        cdn.on("GET", "/files/out.png", [](const HttpRequest &) { return HttpResponse{200, kPng, "image/png"}; });
        // Queue: submit gives the three URLs; the status goes in queue, then runs, then completes.
        for (const char *endpoint : {"fal-ai/flux/dev", "fal-ai/flux-pro/kontext", "fal-ai/flux-pro/v1/fill", "my-org/my-model"})
            api.on("POST", QByteArray("/") + endpoint, [this](const HttpRequest &request) {
                if (!keyed(request))
                    return refused();
                return HttpResponse{200, json({{"request_id", "r1"}, {"response_url", apiUrl("/fal-ai/flux/requests/r1")}, {"status_url", apiUrl("/fal-ai/flux/requests/r1/status")},
                                               {"cancel_url", apiUrl("/fal-ai/flux/requests/r1/cancel")}, {"queue_position", 0}})};
            });
        api.on("GET", "/fal-ai/flux/requests/r1/status", [this](const HttpRequest &request) {
            if (!keyed(request))
                return refused();
            ++polls;
            if (polls == 1)
                return HttpResponse{200, json({{"status", "IN_QUEUE"}, {"queue_position", 2}})};
            if (polls == 2 || endless)
                return HttpResponse{200, json({{"status", "IN_PROGRESS"}})};
            return HttpResponse{200, completed.isEmpty() ? json({{"status", "COMPLETED"}}) : completed};
        });
        api.on("GET", "/fal-ai/flux/requests/r1", [this](const HttpRequest &request) {
            if (!keyed(request))
                return refused();
            return result;
        });
        api.on("PUT", "/fal-ai/flux/requests/r1/cancel", [](const HttpRequest &) { return HttpResponse{200, json({{"status", "OK"}})}; });
        result = HttpResponse{200, json({{"images", QJsonArray{QJsonObject{{"url", cdnUrl("/files/out.png")}, {"width", 1}, {"height", 1}, {"content_type", "image/png"}}}},
                                         {"seed", 42}, {"has_nsfw_concepts", QJsonArray{false}}})};
    }

    bool keyed(const HttpRequest &request) const { return request.headers.value("authorization") == "Key " + kKey; }
    // What a service does with a bad key: it says so and, like some, repeats the key it got.
    HttpResponse refused() const { return HttpResponse{401, json({{"detail", "Invalid credentials: " + QString::fromLatin1(kKey)}})}; }
    QString apiUrl(const QString &path) const { return QString::fromLatin1(api.origin()) + path; }
    QString cdnUrl(const QString &path) const { return QString::fromLatin1(cdn.origin()) + path; }

    std::unique_ptr<FalProvider> provider(const QJsonObject &settings = {}, const QByteArray &key = kKey) {
        ProviderInstance instance;
        instance.name = QStringLiteral("fal.ai");
        instance.type = QStringLiteral("fal");
        instance.address = QString::fromLatin1(api.origin());
        instance.key = QString::fromLatin1(key);
        instance.settings = settings;
        instance.settings.insert("poll_ms", 10);
        return std::make_unique<FalProvider>(instance);
    }

    JobRequest request(const QString &capability, const QString &model, const QJsonObject &params, const QJsonObject &inputs = {}) {
        JobRequest job;
        job.capability = capability;
        job.model = QJsonObject{{"resource", "fugue/image-model"}, {"name", model}};
        job.params = params;
        job.inputs = inputs;
        job.outputDir = dir.filePath("out");
        QDir().mkpath(job.outputDir);
        return job;
    }

    QJsonObject picture(const QString &name, const QByteArray &bytes = kPng) {
        QFile file(dir.filePath(name));
        if (file.open(QIODevice::WriteOnly))
            file.write(bytes);
        return QJsonObject{{"path", dir.filePath(name)}, {"format", "png"}};
    }

    QJsonObject lastBody(const QByteArray &path) const {
        const auto posts = api.requestsTo("POST", path);
        return posts.isEmpty() ? QJsonObject{} : parse(posts.last().body);
    }

    QTemporaryDir dir;
    FakeHttpServer api;
    FakeHttpServer cdn;
    HttpResponse result;
    QByteArray completed; // the status that ends the job, when it is not a plain COMPLETED
    int polls = 0;
    bool endless = false;
};

JobStatus run(FalProvider &provider, const JobRequest &request, JobResult *result = nullptr, QStringList *stages = nullptr) {
    QString jobId;
    QObject::connect(&provider, &Provider::statusChanged, &provider, [stages](const QString &, const JobStatus &status) {
        if (stages && !status.stage.isEmpty() && !stages->contains(status.stage))
            stages->append(status.stage);
    });
    const auto submitted = await<QString>([&](auto reply) { provider.submit(request, reply); });
    JobStatus status;
    if (!submitted.ok()) {
        status.state = QStringLiteral("error");
        status.error = submitted.error();
        return status;
    }
    if (!QTest::qWaitFor([&] {
            status = await<JobStatus>([&](auto reply) { provider.status(submitted.value(), reply); }).value();
            return status.isTerminal();
        }, 8000)) {
        status.state = QStringLiteral("error");
        status.error = makeError(QStringLiteral("internal"), QStringLiteral("The job did not end."));
    }
    if (result && status.state == QLatin1String("done"))
        *result = await<JobResult>([&](auto reply) { provider.result(submitted.value(), reply); }).value();
    return status;
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

const char *const kDev = "fal-ai/flux/dev";

} // namespace

class TestFalProvider : public QObject {
    Q_OBJECT

private slots:
    void theShippedTableOffersItsModelsByWhatTheyDo() {
        Fixture fixture;
        auto provider = fixture.provider({{"image_models", "my-org/my-model, fal-ai/flux/dev"}});
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY2(described.ok(), qPrintable(described.ok() ? QString() : described.error().message));
        QCOMPARE(described->engine, QStringLiteral("fal@127.0.0.1"));
        const auto names = [](const CapabilityOffer *offer) {
            QStringList list;
            for (const OfferedModel &model : offer->models)
                list.append(model.handle.value("name").toString());
            return list;
        };
        const QStringList generate = names(described->offer("image.generate"));
        QVERIFY(generate.contains("fal-ai/flux/schnell") && generate.contains("fal-ai/flux-pro/v1.1") && generate.contains("fal-ai/qwen-image"));
        QVERIFY(generate.contains("my-org/my-model")); // the setting's, with the generic mapping
        QCOMPARE(generate.count("fal-ai/flux/dev"), 1);
        QVERIFY(!generate.contains("fal-ai/flux-pro/kontext")); // an edit model needs its picture
        QCOMPARE(names(described->offer("image.edit")), (QStringList{"fal-ai/flux-pro/kontext", "my-org/my-model"}));
        const OfferedModel *fill = nullptr;
        for (const OfferedModel &model : described->offer("image.generate")->models)
            if (model.handle.value("name").toString() == QLatin1String("fal-ai/flux-pro/v1/fill"))
                fill = &model;
        QVERIFY(fill);
        QCOMPARE(fill->properties.value("inputs").toArray(), QJsonArray{"mask"});
        QCOMPARE(fill->handle, (QJsonObject{{"resource", "fugue/image-model"}, {"name", "fal-ai/flux-pro/v1/fill"}}));
        QCOMPARE(fill->status, ModelStatus::Ready);
        QVERIFY(fill->title.contains("$0.05/MP"));
        QCOMPARE(described->offer("image.generate")->models.first().defaults.value("steps").toInt(), 4);

        // Without a key nothing can run: the models say so, and a job is refused before anything is sent.
        auto keyless = fixture.provider({}, "");
        const auto bare = await<Description>([&](auto reply) { keyless->describe(reply); });
        QCOMPARE(bare->offer("image.generate")->models.first().status, ModelStatus::Unavailable);
        QVERIFY(bare->offer("image.generate")->models.first().hint.contains("Settings > Providers"));
        QCOMPARE(run(*keyless, fixture.request("image.generate", kDev, {{"prompt", "x"}})).error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(fixture.api.requests().isEmpty());
    }

    void aJobGoesThroughTheQueueAndTheKeyStaysOnTheApiHost() {
        Fixture fixture;
        auto provider = fixture.provider();
        JobResult result;
        QStringList stages;
        const JobStatus status = run(*provider, fixture.request("image.generate", kDev,
                                                                {{"prompt", "a dot"}, {"width", 768}, {"height", 512}, {"seed", 7}, {"steps", 20}, {"guidance", 4.0}}),
                                     &result, &stages);
        QCOMPARE(status.state, QStringLiteral("done"));
        QVERIFY(stages.contains("in queue, position 3"));
        QVERIFY(stages.contains("running"));

        const QJsonObject body = fixture.lastBody("/fal-ai/flux/dev");
        QCOMPARE(body.value("prompt").toString(), QStringLiteral("a dot"));
        QCOMPARE(body.value("image_size").toObject(), (QJsonObject{{"width", 768}, {"height", 512}}));
        QCOMPARE(body.value("seed").toInt(), 7);
        QCOMPARE(body.value("num_inference_steps").toInt(), 20);
        QCOMPARE(body.value("guidance_scale").toDouble(), 4.0);
        QCOMPARE(body.value("num_images").toInt(), 1);
        QCOMPARE(body.value("output_format").toString(), QStringLiteral("png"));
        QVERIFY(body.value("enable_safety_checker").toBool());
        QVERIFY(!body.contains("image_url"));
        // Privacy: fal is asked to keep nothing, and to expire what it holds.
        const HttpRequest submitted = fixture.api.requestsTo("POST", "/fal-ai/flux/dev").last();
        QCOMPARE(submitted.headers.value("x-fal-store-io"), QByteArray("0"));
        QVERIFY(submitted.headers.contains("x-fal-object-lifecycle-preference"));

        // The result: the picture, the service's seed, the values used.
        QCOMPARE(readAll(result.outputs.value("image").toObject().value("path").toString()), kPng);
        QCOMPARE(result.used.value("seed").toInt(), 42);
        QCOMPARE(result.used.value("engine").toString(), QStringLiteral("fal@fal-ai/flux/dev"));
        QCOMPARE(result.used.value("model").toObject().value("name").toString(), QString(kDev));
        QCOMPARE(result.used.value("steps").toInt(), 20);

        // The key reached the queue (every call) and nothing else.
        QVERIFY(!fixture.api.requests().isEmpty());
        for (const HttpRequest &request : fixture.api.requests())
            QCOMPARE(request.headers.value("authorization"), "Key " + kKey);
        QVERIFY(!fixture.cdn.requests().isEmpty());
        for (const HttpRequest &request : fixture.cdn.requests())
            QVERIFY2(!request.headers.contains("authorization"), "the key went to the file server");
        // No message, status or result holds it.
        QVERIFY(!json(result.used).contains(kKey));
        QVERIFY(!json(status.error.toJson()).contains(kKey));
    }

    void anEditUploadsItsPictureFirstAndKeepsItForTheSession() {
        Fixture fixture;
        auto provider = fixture.provider();
        const QJsonObject image = fixture.picture("start.png");
        JobRequest edit = fixture.request("image.edit", "fal-ai/flux-pro/kontext", {{"prompt", "make it night"}, {"guidance", 3.5}}, {{"images", QJsonArray{image}}});
        QCOMPARE(run(*provider, edit).state, QStringLiteral("done"));
        const HttpRequest initiated = fixture.api.requestsTo("POST", "/storage/upload/initiate?storage_type=fal-cdn-v3").last();
        QCOMPARE(parse(initiated.body).value("content_type").toString(), QStringLiteral("image/png"));
        QVERIFY(initiated.headers.contains("x-fal-object-lifecycle"));
        const auto puts = fixture.cdn.requestsTo("PUT", "/upload/image.png");
        QCOMPARE(puts.size(), 1);
        QCOMPARE(puts.first().body, kPng);
        QCOMPARE(puts.first().headers.value("content-type"), QByteArray("image/png"));
        const QJsonObject body = fixture.lastBody("/fal-ai/flux-pro/kontext");
        QCOMPARE(body.value("image_url").toString(), fixture.cdnUrl("/files/in-image.png"));
        QCOMPARE(body.value("prompt").toString(), QStringLiteral("make it night"));
        QVERIFY(!body.contains("image_size")); // Kontext follows its picture

        // The same picture again is not sent again.
        fixture.polls = 0;
        QCOMPARE(run(*provider, edit).state, QStringLiteral("done"));
        QCOMPARE(fixture.api.requestsTo("POST", "/storage/upload/initiate?storage_type=fal-cdn-v3").size(), 1);
        QCOMPARE(fixture.api.requestsTo("POST", "/fal-ai/flux-pro/kontext").size(), 2);

        // A model with no picture field refuses a start picture; an edit with none is missing it.
        QCOMPARE(run(*provider, fixture.request("image.generate", kDev, {{"prompt", "x"}}, {{"image", image}})).error.code, QStringLiteral("input_invalid"));
        QCOMPARE(run(*provider, fixture.request("image.edit", "fal-ai/flux-pro/kontext", {{"prompt", "x"}})).error.code, QStringLiteral("missing_input"));
    }

    void anInpaintSendsThePictureAndTheMaskAndOnlyToAModelThatTakesOne() {
        Fixture fixture;
        auto provider = fixture.provider();
        const QJsonObject image = fixture.picture("a.png");
        const QJsonObject mask = fixture.picture("m.png", kPng + "mask");
        const JobStatus status = run(*provider, fixture.request("image.generate", "fal-ai/flux-pro/v1/fill", {{"prompt", "a hat"}}, {{"image", image}, {"mask", mask}}));
        QCOMPARE(status.state, QStringLiteral("done"));
        const QJsonObject body = fixture.lastBody("/fal-ai/flux-pro/v1/fill");
        QCOMPARE(body.value("image_url").toString(), fixture.cdnUrl("/files/in-image.png"));
        QCOMPARE(body.value("mask_url").toString(), fixture.cdnUrl("/files/in-mask.png"));
        QCOMPARE(fixture.cdn.requestsTo("PUT", "/upload/mask.png").size(), 1);

        QCOMPARE(run(*provider, fixture.request("image.generate", kDev, {{"prompt", "x"}}, {{"image", image}, {"mask", mask}})).error.code, QStringLiteral("mask_unsupported"));
        QCOMPARE(run(*provider, fixture.request("image.generate", "fal-ai/flux-pro/v1/fill", {{"prompt", "x"}}, {{"image", image}})).error.code, QStringLiteral("missing_input"));
    }

    void cancelAsksTheQueueAtTheUrlItReturned() {
        Fixture fixture;
        fixture.endless = true;
        auto provider = fixture.provider();
        const auto job = await<QString>([&](auto reply) { provider->submit(fixture.request("image.generate", kDev, {{"prompt", "x"}}), reply); });
        QVERIFY(job.ok());
        QTRY_VERIFY(fixture.polls >= 2);
        const auto cancelled = await<QString>([&](auto reply) { provider->cancel(job.value(), reply); });
        QCOMPARE(cancelled.value(), QStringLiteral("cancelled"));
        QTRY_COMPARE(fixture.api.requestsTo("PUT", "/fal-ai/flux/requests/r1/cancel").size(), 1);
        QCOMPARE(fixture.api.requestsTo("PUT", "/fal-ai/flux/requests/r1/cancel").first().headers.value("authorization"), "Key " + kKey);
        const int seen = fixture.polls;
        QTest::qWait(60);
        QCOMPARE(fixture.polls, seen); // it is no longer followed
        QCOMPARE(await<JobStatus>([&](auto reply) { provider->status(job.value(), reply); }).value().state, QStringLiteral("cancelled"));
    }

    void theServiceNeverSendsTheKeyToAnotherHostOrFollowsARedirect() {
        Fixture fixture;
        // A status_url on another host (here the file server) is refused before the key can go there.
        fixture.api.on("POST", "/fal-ai/flux/dev", [&](const HttpRequest &) {
            return HttpResponse{200, json({{"request_id", "r1"}, {"response_url", fixture.cdnUrl("/r")}, {"status_url", fixture.cdnUrl("/s")}, {"cancel_url", fixture.cdnUrl("/c")}})};
        });
        auto provider = fixture.provider();
        const JobStatus status = run(*provider, fixture.request("image.generate", kDev, {{"prompt", "x"}}));
        QCOMPARE(status.error.code, QStringLiteral("node_failed"));
        QVERIFY(fixture.cdn.requests().isEmpty());

        // An API call that is redirected is an error: the redirect is not followed.
        fixture.api.on("POST", "/fal-ai/flux/dev", [&](const HttpRequest &) {
            return HttpResponse{302, "", "text/plain", false, -1, {"Location: " + fixture.cdnUrl("/elsewhere").toLatin1()}};
        });
        QCOMPARE(run(*provider, fixture.request("image.generate", kDev, {{"prompt", "x"}})).error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(fixture.cdn.requests().isEmpty());
    }

    void errorsMapToTheCodesOfTheSpecAndNeverCarryTheKey() {
        struct Row {
            const char *name;
            HttpResponse answer;
            QString code;
            QString says;
        };
        const QString echo = QString::fromLatin1(kKey);
        const QList<Row> rows{
            {"401", HttpResponse{401, json({{"detail", "bad key " + echo}})}, "provider_unreachable", "refused the key"},
            {"403", HttpResponse{403, json({{"detail", "User is locked. Reason: Exhausted balance."}})}, "provider_unreachable", "Exhausted balance"},
            {"404", HttpResponse{404, json({{"detail", "Not found"}})}, "model_missing", "no such model"},
            {"422", HttpResponse{422, json({{"detail", QJsonArray{QJsonObject{{"loc", QJsonArray{"body", "image_size"}}, {"msg", "image_size is too large"}, {"type", "value_error"}}}}})},
             "node_failed", "image_size: image_size is too large"},
            {"policy", HttpResponse{422, json({{"detail", QJsonArray{QJsonObject{{"msg", "flagged"}, {"type", "content_policy_violation"}}}}})}, "node_failed", "content policy"},
            {"429", HttpResponse{429, json({{"detail", "slow down"}}), "application/json", false, -1, {"Retry-After: 17"}}, "node_failed", "wait 17 seconds"},
            {"500", HttpResponse{500, json({{"detail", "boom " + echo}})}, "node_failed", "boom"},
            {"oom", HttpResponse{500, json({{"detail", "CUDA out of memory"}})}, "out_of_memory", "out of memory"},
            {"502", HttpResponse{502, "{}"}, "provider_unreachable", "not available"},
            {"504", HttpResponse{504, "{}"}, "provider_unreachable", "not available"},
            {"400", HttpResponse{400, json({{"detail", "bad_request"}})}, "node_failed", "bad_request"},
        };
        for (const Row &row : rows) {
            Fixture fixture;
            fixture.api.on("POST", "/fal-ai/flux/dev", [&row](const HttpRequest &) { return row.answer; });
            auto provider = fixture.provider();
            const JobStatus status = run(*provider, fixture.request("image.generate", kDev, {{"prompt", "x"}}));
            QVERIFY2(status.state == QLatin1String("error"), row.name);
            QVERIFY2(status.error.code == row.code, qPrintable(QStringLiteral("%1: %2 (%3)").arg(row.name, status.error.code, status.error.message)));
            QVERIFY2(status.error.message.contains(row.says, Qt::CaseInsensitive), qPrintable(QStringLiteral("%1: %2").arg(row.name, status.error.message)));
            const QByteArray said = json(status.error.toJson());
            QVERIFY2(!said.contains(kKey) && !said.contains("secretsecret"), row.name);
            if (QLatin1String(row.name) == QLatin1String("401"))
                QVERIFY(status.error.hint.contains("Settings > Providers"));
        }
    }

    void aJobTheQueueEndsWithAnErrorOrAResultItRefusesIsAFailure() {
        {
            Fixture fixture; // COMPLETED with an error
            fixture.completed = json({{"status", "COMPLETED"}, {"error", "runner crashed " + QString::fromLatin1(kKey)}, {"error_type", "runner_error"}});
            auto provider = fixture.provider();
            const JobStatus status = run(*provider, fixture.request("image.generate", kDev, {{"prompt", "x"}}));
            QCOMPARE(status.error.code, QStringLiteral("node_failed"));
            QVERIFY(status.error.message.contains("runner crashed"));
            QVERIFY(!status.error.message.contains("secretsecret"));
            QVERIFY(fixture.api.requestsTo("GET", "/fal-ai/flux/requests/r1").isEmpty());
        }
        {
            Fixture fixture; // the result is a 422
            fixture.result = HttpResponse{422, json({{"detail", QJsonArray{QJsonObject{{"msg", "no media"}, {"type", "no_media_generated"}}}}})};
            auto provider = fixture.provider();
            const JobStatus status = run(*provider, fixture.request("image.generate", kDev, {{"prompt", "x"}}));
            QCOMPARE(status.error.code, QStringLiteral("node_failed"));
            QVERIFY(status.error.message.contains("no picture"));
        }
        {
            Fixture fixture; // a picture the safety checker blanked
            fixture.result = HttpResponse{200, json({{"images", QJsonArray{QJsonObject{{"url", fixture.cdnUrl("/files/out.png")}}}}, {"has_nsfw_concepts", QJsonArray{true}}})};
            auto provider = fixture.provider();
            QVERIFY(run(*provider, fixture.request("image.generate", kDev, {{"prompt", "x"}})).error.message.contains("safety checker"));
        }
        {
            Fixture fixture; // an expired picture link
            fixture.cdn.on("GET", "/files/out.png", [](const HttpRequest &) { return HttpResponse{403, "expired"}; });
            auto provider = fixture.provider();
            QVERIFY(run(*provider, fixture.request("image.generate", kDev, {{"prompt", "x"}})).error.message.contains("expired"));
        }
    }

    void aQueueThatSaysNothingIsATimeoutNamingTheSetting() {
        Fixture fixture;
        fixture.api.on("POST", "/fal-ai/flux/dev", [](const HttpRequest &) { return HttpResponse{200, {}, "application/json", true}; });
        auto provider = fixture.provider({{"timeout_s", 0.2}});
        const JobStatus status = run(*provider, fixture.request("image.generate", kDev, {{"prompt", "x"}}));
        QCOMPARE(status.error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(status.error.hint.contains("timeout_s"));
    }

    void storeIoTurnsThePrivacyHeadersOff() {
        Fixture fixture;
        auto provider = fixture.provider({{"store_io", true}});
        QCOMPARE(run(*provider, fixture.request("image.generate", kDev, {{"prompt", "x"}})).state, QStringLiteral("done"));
        const HttpRequest submitted = fixture.api.requestsTo("POST", "/fal-ai/flux/dev").last();
        QVERIFY(!submitted.headers.contains("x-fal-store-io"));
    }

    void aFalInstanceIsMadeByTheRegistry() {
        ProviderInstance instance;
        instance.name = QStringLiteral("fal.ai");
        instance.type = QStringLiteral("fal");
        instance.key = "k";
        const auto provider = createProvider(instance, QDir::tempPath() + "/no-such-modules");
        QVERIFY(provider.ok());
        QVERIFY(dynamic_cast<FalProvider *>(provider.value().get()));
        QVERIFY(falShippedModels().size() >= 6);
    }
};

FUGUE_TEST_MAIN(TestFalProvider, QCoreApplication)
#include "test_fal_provider.moc"
