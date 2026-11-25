// The `replicate` provider (spec 8.7, #407) against a fake Replicate API and a fake delivery host (the CDN) on the loopback.
// Every key here is made up. Nothing reaches Replicate.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/providers.h"
#include "engine/replicate_provider.h"
#include "fake_http.h"
#include "../test_shard.h"
#include "provider_support.h"

using namespace fugue;
using fugue::test::await;
using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

const QByteArray kKey = "r8_test0123456789abcdefsecretsecret";

// A 1x1 PNG.
const QByteArray kPng = QByteArray::fromBase64(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");

QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject parse(const QByteArray &bytes) { return QJsonDocument::fromJson(bytes).object(); }

const char *const kSchnell = "black-forest-labs/flux-schnell";
const char *const kKontext = "black-forest-labs/flux-kontext-dev";
const char *const kFill = "black-forest-labs/flux-fill-dev";
const char *const kKlein = "black-forest-labs/flux-2-klein-4b";

// The API (predictions and files) and the delivery host (it must never see the key).
class Fixture {
public:
    Fixture() {
        api.listen();
        cdn.listen();
        for (const char *model : {kSchnell, kKontext, kFill, kKlein, "qwen/qwen-image", "my-org/my-model"})
            api.on("POST", QByteArray("/v1/models/") + model + "/predictions", [this](const HttpRequest &request) {
                if (!keyed(request))
                    return refused();
                return HttpResponse{201, json({{"id", "p1"}, {"status", "starting"},
                                               {"urls", QJsonObject{{"get", apiUrl("/v1/predictions/p1")}, {"cancel", apiUrl("/v1/predictions/p1/cancel")}}}})};
            });
        api.on("GET", "/v1/predictions/p1", [this](const HttpRequest &request) {
            if (!keyed(request))
                return refused();
            ++polls;
            if (polls == 1)
                return HttpResponse{200, json({{"id", "p1"}, {"status", "starting"}})};
            if (polls == 2 || endless)
                return HttpResponse{200, json({{"id", "p1"}, {"status", "processing"}})};
            return HttpResponse{200, finished.isEmpty() ? json({{"id", "p1"}, {"status", "succeeded"}, {"output", QJsonArray{cdnUrl("/out.png")}}}) : finished};
        });
        api.on("POST", "/v1/predictions/p1/cancel", [](const HttpRequest &) { return HttpResponse{200, json({{"status", "canceled"}})}; });
        api.on("POST", "/v1/files", [this](const HttpRequest &request) {
            if (!keyed(request))
                return refused();
            return HttpResponse{201, json({{"id", "f1"}, {"urls", QJsonObject{{"get", apiUrl("/v1/files/f1")}}}})};
        });
        cdn.on("GET", "/out.png", [](const HttpRequest &) { return HttpResponse{200, kPng, "image/png"}; });
    }

    bool keyed(const HttpRequest &request) const { return request.headers.value("authorization") == "Bearer " + kKey; }
    // What a service does with a bad key: it says so and, like some, repeats the key it got.
    HttpResponse refused() const { return HttpResponse{401, json({{"title", "Unauthenticated"}, {"detail", "Invalid token: " + QString::fromLatin1(kKey)}, {"status", 401}})}; }
    QString apiUrl(const QString &path) const { return QString::fromLatin1(api.origin()) + path; }
    QString cdnUrl(const QString &path) const { return QString::fromLatin1(cdn.origin()) + path; }

    std::unique_ptr<ReplicateProvider> provider(const QJsonObject &settings = {}, const QByteArray &key = kKey) {
        ProviderInstance instance;
        instance.name = QStringLiteral("Replicate");
        instance.type = QStringLiteral("replicate");
        instance.address = QString::fromLatin1(api.origin());
        instance.key = QString::fromLatin1(key);
        instance.settings = settings;
        instance.settings.insert("poll_ms", 10);
        return std::make_unique<ReplicateProvider>(instance);
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

    // The `input` of the last prediction made for a model.
    QJsonObject lastInput(const QByteArray &model) const {
        const auto posts = api.requestsTo("POST", "/v1/models/" + model + "/predictions");
        return posts.isEmpty() ? QJsonObject{} : parse(posts.last().body).value("input").toObject();
    }

    QTemporaryDir dir;
    FakeHttpServer api;
    FakeHttpServer cdn;
    QByteArray finished; // the poll that ends the job, when it is not a plain success
    int polls = 0;
    bool endless = false;
};

JobStatus run(ReplicateProvider &provider, const JobRequest &request, JobResult *result = nullptr, QStringList *stages = nullptr) {
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

} // namespace

class TestReplicateProvider : public QObject {
    Q_OBJECT

private slots:
    void theShippedTableOffersItsModelsByWhatTheyDo() {
        Fixture fixture;
        auto provider = fixture.provider({{"image_models", "my-org/my-model, black-forest-labs/flux-schnell, not an id"}});
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY2(described.ok(), qPrintable(described.ok() ? QString() : described.error().message));
        QCOMPARE(described->engine, QStringLiteral("replicate@127.0.0.1"));
        const auto names = [](const CapabilityOffer *offer) {
            QStringList list;
            for (const OfferedModel &model : offer->models)
                list.append(model.handle.value("name").toString());
            return list;
        };
        const QStringList generate = names(described->offer("image.generate"));
        QVERIFY(generate.contains(kSchnell) && generate.contains("qwen/qwen-image") && generate.contains(kKlein));
        QVERIFY(generate.contains("my-org/my-model") && !generate.contains("not an id"));
        QCOMPARE(generate.count(kSchnell), 1);
        QVERIFY(!generate.contains(kKontext)); // an edit model needs its picture
        QCOMPARE(names(described->offer("image.edit")), (QStringList{kKlein, kKontext, "qwen/qwen-image-edit", "my-org/my-model"}));
        const OfferedModel *fill = nullptr;
        for (const OfferedModel &model : described->offer("image.generate")->models)
            if (model.handle.value("name").toString() == QLatin1String(kFill))
                fill = &model;
        QVERIFY(fill);
        QCOMPARE(fill->properties.value("inputs").toArray(), QJsonArray{"mask"});
        QCOMPARE(fill->status, ModelStatus::Ready);
        QVERIFY(fill->title.contains("$0.04/image"));
        QCOMPARE(described->offer("image.generate")->models.first().defaults.value("steps").toInt(), 4);

        // Without a key nothing can run: the models say so, and a job is refused before anything is sent.
        auto keyless = fixture.provider({}, "");
        const auto bare = await<Description>([&](auto reply) { keyless->describe(reply); });
        QCOMPARE(bare->offer("image.generate")->models.first().status, ModelStatus::Unavailable);
        QVERIFY(bare->offer("image.generate")->models.first().hint.contains("Settings > Providers"));
        QCOMPARE(run(*keyless, fixture.request("image.generate", kSchnell, {{"prompt", "x"}})).error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(fixture.api.requests().isEmpty());
    }

    void aJobIsCreatedPolledAndDownloadedAndTheKeyStaysOnTheApi() {
        Fixture fixture;
        auto provider = fixture.provider();
        JobResult result;
        QStringList stages;
        const JobStatus status = run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "a dot"}, {"width", 1344}, {"height", 768}, {"seed", 7}, {"steps", 20}}),
                                     &result, &stages);
        QCOMPARE(status.state, QStringLiteral("done"));
        QVERIFY(stages.contains("starting"));
        QVERIFY(stages.contains("running"));

        const QJsonObject input = fixture.lastInput(kSchnell);
        QCOMPARE(input.value("prompt").toString(), QStringLiteral("a dot"));
        QCOMPARE(input.value("aspect_ratio").toString(), QStringLiteral("16:9")); // the nearest the model has
        QCOMPARE(input.value("megapixels").toString(), QStringLiteral("1"));
        QCOMPARE(input.value("seed").toInt(), 7);
        QCOMPARE(input.value("num_inference_steps").toInt(), 4); // cut to the model's 4
        QCOMPARE(input.value("output_format").toString(), QStringLiteral("png"));
        QVERIFY(!input.contains("image"));
        // Replicate is asked to stop the job at the deadline itself.
        QVERIFY(fixture.api.requestsTo("POST", "/v1/models/black-forest-labs/flux-schnell/predictions").last().headers.contains("cancel-after"));

        QCOMPARE(readAll(result.outputs.value("image").toObject().value("path").toString()), kPng);
        QCOMPARE(result.used.value("seed").toInt(), 7);
        QCOMPARE(result.used.value("engine").toString(), QStringLiteral("replicate@black-forest-labs/flux-schnell"));
        QCOMPARE(result.used.value("steps").toInt(), 4);

        // The key reached the API (every call) and nothing else.
        QVERIFY(!fixture.api.requests().isEmpty());
        for (const HttpRequest &request : fixture.api.requests())
            QCOMPARE(request.headers.value("authorization"), "Bearer " + kKey);
        QVERIFY(!fixture.cdn.requests().isEmpty());
        for (const HttpRequest &request : fixture.cdn.requests())
            QVERIFY2(!request.headers.contains("authorization"), "the key went to the delivery host");
        QVERIFY(!json(result.used).contains(kKey));
    }

    void anEditSendsItsPictureAsADataUriAndABigOneThroughTheFilesApi() {
        Fixture fixture;
        auto provider = fixture.provider();
        const QJsonObject image = fixture.picture("start.png");
        JobRequest edit = fixture.request("image.edit", kKontext, {{"prompt", "make it night"}, {"guidance", 3.5}, {"width", 512}, {"height", 512}}, {{"images", QJsonArray{image}}});
        QCOMPARE(run(*provider, edit).state, QStringLiteral("done"));
        const QJsonObject input = fixture.lastInput(kKontext);
        QVERIFY(input.value("input_image").toString().startsWith("data:image/png;base64,"));
        QCOMPARE(input.value("guidance").toDouble(), 3.5);
        QVERIFY(!input.contains("aspect_ratio")); // Kontext follows its picture
        QVERIFY(fixture.api.requestsTo("POST", "/v1/files").isEmpty());

        // Klein takes a list and follows its picture's shape.
        fixture.polls = 0;
        QCOMPARE(run(*provider, fixture.request("image.edit", kKlein, {{"prompt", "x"}, {"width", 512}, {"height", 512}}, {{"images", QJsonArray{image}}})).state, QStringLiteral("done"));
        const QJsonObject klein = fixture.lastInput(kKlein);
        QCOMPARE(klein.value("images").toArray().size(), 1);
        QCOMPARE(klein.value("aspect_ratio").toString(), QStringLiteral("match_input_image"));
        QVERIFY(!klein.contains("output_megapixels"));

        // Over 1 MiB: the files API (the key goes to it, as multipart), and its link goes into the input.
        fixture.polls = 0;
        QByteArray big = kPng;
        big.append(QByteArray(1100 * 1024, 'x'));
        const QJsonObject large = fixture.picture("big.png", big);
        QCOMPARE(run(*provider, fixture.request("image.edit", kKontext, {{"prompt", "x"}}, {{"images", QJsonArray{large}}})).state, QStringLiteral("done"));
        const auto uploads = fixture.api.requestsTo("POST", "/v1/files");
        QCOMPARE(uploads.size(), 1);
        QVERIFY(uploads.first().headers.value("content-type").startsWith("multipart/form-data"));
        QVERIFY(uploads.first().body.contains("name=\"content\""));
        QCOMPARE(fixture.lastInput(kKontext).value("input_image").toString(), fixture.apiUrl("/v1/files/f1"));

        // A model with no picture field refuses a start picture; an edit with none is missing it.
        QCOMPARE(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}, {{"image", image}})).error.code, QStringLiteral("input_invalid"));
        QCOMPARE(run(*provider, fixture.request("image.edit", kKontext, {{"prompt", "x"}})).error.code, QStringLiteral("missing_input"));
    }

    void anInpaintSendsThePictureAndTheMaskAndOnlyToAModelThatTakesOne() {
        Fixture fixture;
        auto provider = fixture.provider();
        const QJsonObject image = fixture.picture("a.png");
        const QJsonObject mask = fixture.picture("m.png", kPng + "mask");
        QCOMPARE(run(*provider, fixture.request("image.generate", kFill, {{"prompt", "a hat"}, {"guidance", 20.0}}, {{"image", image}, {"mask", mask}})).state, QStringLiteral("done"));
        const QJsonObject input = fixture.lastInput(kFill);
        QCOMPARE(input.value("image").toString(), "data:image/png;base64," + QString::fromLatin1(kPng.toBase64()));
        QVERIFY(input.value("mask").toString().startsWith("data:image/png;base64,"));
        QVERIFY(input.value("mask").toString() != input.value("image").toString());
        QCOMPARE(input.value("megapixels").toString(), QStringLiteral("match_input"));
        QCOMPARE(input.value("guidance").toDouble(), 20.0);

        QCOMPARE(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}, {{"image", image}, {"mask", mask}})).error.code, QStringLiteral("mask_unsupported"));
        QCOMPARE(run(*provider, fixture.request("image.generate", kFill, {{"prompt", "x"}}, {{"image", image}})).error.code, QStringLiteral("missing_input"));
    }

    void cancelPostsToTheUrlItReturned() {
        Fixture fixture;
        fixture.endless = true;
        auto provider = fixture.provider();
        const auto job = await<QString>([&](auto reply) { provider->submit(fixture.request("image.generate", kSchnell, {{"prompt", "x"}}), reply); });
        QVERIFY(job.ok());
        QTRY_VERIFY(fixture.polls >= 2);
        const auto cancelled = await<QString>([&](auto reply) { provider->cancel(job.value(), reply); });
        QCOMPARE(cancelled.value(), QStringLiteral("cancelled"));
        QTRY_COMPARE(fixture.api.requestsTo("POST", "/v1/predictions/p1/cancel").size(), 1);
        QCOMPARE(fixture.api.requestsTo("POST", "/v1/predictions/p1/cancel").first().headers.value("authorization"), "Bearer " + kKey);
        const int seen = fixture.polls;
        QTest::qWait(60);
        QCOMPARE(fixture.polls, seen); // it is no longer followed
    }

    void theKeyNeverGoesToAnotherHostNorFollowsARedirect() {
        Fixture fixture;
        // A urls.get on another host (here the delivery host) is refused before the key can go there.
        fixture.api.on("POST", "/v1/models/black-forest-labs/flux-schnell/predictions", [&](const HttpRequest &) {
            return HttpResponse{201, json({{"id", "p1"}, {"urls", QJsonObject{{"get", fixture.cdnUrl("/s")}, {"cancel", fixture.cdnUrl("/c")}}}})};
        });
        auto provider = fixture.provider();
        QCOMPARE(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}})).error.code, QStringLiteral("node_failed"));
        QVERIFY(fixture.cdn.requests().isEmpty());

        // An API call that is redirected is an error: the redirect is not followed.
        fixture.api.on("POST", "/v1/models/black-forest-labs/flux-schnell/predictions", [&](const HttpRequest &) {
            return HttpResponse{302, "", "text/plain", false, -1, {"Location: " + fixture.cdnUrl("/elsewhere").toLatin1()}};
        });
        QCOMPARE(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}})).error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(fixture.cdn.requests().isEmpty());

        // An output that is not an http(s) link is not fetched.
        Fixture second;
        second.finished = json({{"status", "succeeded"}, {"output", "file:///etc/hostname"}});
        auto other = second.provider();
        QCOMPARE(run(*other, second.request("image.generate", kSchnell, {{"prompt", "x"}})).error.code, QStringLiteral("node_failed"));
    }

    void theKeyIsNotSentOverPlainHttpToARemoteHost() {
        ProviderInstance instance;
        instance.name = QStringLiteral("Replicate");
        instance.type = QStringLiteral("replicate");
        instance.key = QString::fromLatin1(kKey);
        instance.address = QStringLiteral("http://api.example.com");
        ReplicateProvider provider(instance);
        const auto described = await<Description>([&](auto reply) { provider.describe(reply); });
        QCOMPARE(described.error().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.error().hint.contains("https://"));
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
            {"401", HttpResponse{401, json({{"detail", "bad token " + echo}})}, "provider_unreachable", "refused the key"},
            {"402", HttpResponse{402, json({{"title", "Insufficient credit"}, {"detail", "You have insufficient credit to run this model."}})}, "provider_unreachable", "no credit"},
            {"404", HttpResponse{404, json({{"detail", "The requested resource could not be found."}})}, "model_missing", "no such model"},
            {"422", HttpResponse{422, json({{"detail", "Input validation failed"}, {"invalid_fields", QJsonArray{QJsonObject{{"field", "input.aspect_ratio"}, {"description", "not a valid choice"}}}}})},
             "node_failed", "input.aspect_ratio: not a valid choice"},
            {"429", HttpResponse{429, json({{"detail", "Request was throttled. Expected available in 1 second."}}), "application/json", false, -1, {"Retry-After: 17"}}, "node_failed", "wait 17 seconds"},
            {"429 bare", HttpResponse{429, json({{"detail", "Request was throttled. Expected available in 1 second."}})}, "node_failed", "throttled"},
            {"500", HttpResponse{500, json({{"detail", "boom " + echo}})}, "node_failed", "boom"},
            {"oom", HttpResponse{500, json({{"detail", "CUDA out of memory"}})}, "out_of_memory", "out of memory"},
            {"503", HttpResponse{503, "{}"}, "provider_unreachable", "not available"},
            {"400", HttpResponse{400, json({{"detail", "bad_request"}})}, "node_failed", "bad_request"},
        };
        for (const Row &row : rows) {
            Fixture fixture;
            fixture.api.on("POST", "/v1/models/black-forest-labs/flux-schnell/predictions", [&row](const HttpRequest &) { return row.answer; });
            auto provider = fixture.provider();
            const JobStatus status = run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}));
            QVERIFY2(status.state == QLatin1String("error"), row.name);
            QVERIFY2(status.error.code == row.code, qPrintable(QStringLiteral("%1: %2 (%3)").arg(row.name, status.error.code, status.error.message)));
            QVERIFY2(status.error.message.contains(row.says, Qt::CaseInsensitive), qPrintable(QStringLiteral("%1: %2").arg(row.name, status.error.message)));
            const QByteArray said = json(status.error.toJson());
            QVERIFY2(!said.contains(kKey) && !said.contains("secretsecret"), row.name);
            if (QLatin1String(row.name) == QLatin1String("401"))
                QVERIFY(status.error.hint.contains("Settings > Providers"));
            if (QLatin1String(row.name) == QLatin1String("402"))
                QVERIFY(status.error.hint.contains("billing"));
        }
    }

    void aPredictionThatFailsOrAnOutputItCannotUseIsAFailure() {
        {
            Fixture fixture; // failed, with an error that repeats the key
            fixture.finished = json({{"status", "failed"}, {"error", "runner crashed " + QString::fromLatin1(kKey)}});
            auto provider = fixture.provider();
            const JobStatus status = run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}));
            QCOMPARE(status.error.code, QStringLiteral("node_failed"));
            QVERIFY(status.error.message.contains("runner crashed"));
            QVERIFY(!status.error.message.contains("secretsecret"));
        }
        {
            Fixture fixture; // E1001
            fixture.finished = json({{"status", "failed"}, {"error", "Prediction failed: E1001 out of memory"}});
            auto provider = fixture.provider();
            QCOMPARE(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}})).error.code, QStringLiteral("out_of_memory"));
        }
        {
            Fixture fixture; // the safety checker
            fixture.finished = json({{"status", "failed"}, {"error", "NSFW content detected. Try running it again, or try a different prompt."}});
            auto provider = fixture.provider();
            QVERIFY(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}})).error.message.contains("content policy"));
        }
        {
            Fixture fixture; // succeeded with nothing
            fixture.finished = json({{"status", "succeeded"}, {"output", QJsonValue::Null}});
            auto provider = fixture.provider();
            QVERIFY(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}})).error.message.contains("without an image"));
        }
        {
            Fixture fixture; // an expired picture link
            fixture.cdn.on("GET", "/out.png", [](const HttpRequest &) { return HttpResponse{404, "expired"}; });
            auto provider = fixture.provider();
            QVERIFY(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}})).error.message.contains("expired"));
        }
    }

    void aServiceThatSaysNothingIsATimeoutNamingTheSetting() {
        Fixture fixture;
        fixture.api.on("POST", "/v1/models/black-forest-labs/flux-schnell/predictions", [](const HttpRequest &) { return HttpResponse{200, {}, "application/json", true}; });
        auto provider = fixture.provider({{"timeout_s", 0.2}});
        const JobStatus status = run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}));
        QCOMPARE(status.error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(status.error.hint.contains("timeout_s"));
    }

    void aJobThatNeverEndsIsGivenUpOnAtItsDeadline() {
        Fixture fixture;
        fixture.endless = true;
        auto provider = fixture.provider({{"timeout_s", 0.1}}); // the job's deadline is 6 times that
        const JobStatus status = run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}));
        QCOMPARE(status.error.code, QStringLiteral("node_timeout"));
        QTRY_COMPARE(fixture.api.requestsTo("POST", "/v1/predictions/p1/cancel").size(), 1);
    }

    void aReplicateInstanceIsMadeByTheRegistry() {
        ProviderInstance instance;
        instance.name = QStringLiteral("Replicate");
        instance.type = QStringLiteral("replicate");
        instance.key = "k";
        const auto provider = createProvider(instance, QDir::tempPath() + "/no-such-modules");
        QVERIFY(provider.ok());
        QVERIFY(dynamic_cast<ReplicateProvider *>(provider.value().get()));
        QCOMPARE(replicateShippedModels().size(), 6);
        QCOMPARE(replicateAddress(instance), QStringLiteral("https://api.replicate.com"));
    }
};

FUGUE_TEST_MAIN(TestReplicateProvider, QCoreApplication)
#include "test_replicate_provider.moc"
