// The `gemini` provider (spec 8.7, #407) against a fake Gemini API on the loopback. Every key here is made up.
// Nothing reaches Google.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/gemini_provider.h"
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

const QByteArray kKey = "AIzaTestKey0123456789abcdefghijklmnop";

// A 1x1 PNG.
const QByteArray kPng = QByteArray::fromBase64(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");

QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject parse(const QByteArray &bytes) { return QJsonDocument::fromJson(bytes).object(); }

QJsonObject imagePart(const QByteArray &bytes, bool thought = false) {
    QJsonObject part{{"inlineData", QJsonObject{{"mimeType", "image/png"}, {"data", QString::fromLatin1(bytes.toBase64())}}}};
    if (thought)
        part.insert("thought", true);
    return part;
}

HttpResponse answer(const QJsonArray &parts, const QString &finish = QStringLiteral("STOP")) {
    return HttpResponse{200, json({{"candidates", QJsonArray{QJsonObject{{"content", QJsonObject{{"role", "model"}, {"parts", parts}}}, {"finishReason", finish}}}}})};
}

const char *const kPath = "/v1beta/models/gemini-3.1-flash-image:generateContent";

class Fixture {
public:
    Fixture() {
        api.listen();
        api.on("POST", kPath, [this](const HttpRequest &request) {
            if (request.headers.value("x-goog-api-key") != kKey)
                return HttpResponse{400, json({{"error", QJsonObject{{"code", 400}, {"message", "API key not valid. Please pass a valid API key: " + QString::fromLatin1(kKey)},
                                                                     {"status", "INVALID_ARGUMENT"}}}})};
            return reply;
        });
        reply = answer({QJsonObject{{"text", "Here you go."}}, imagePart(kPng)});
    }

    std::unique_ptr<GeminiProvider> provider(const QJsonObject &settings = {}, const QByteArray &key = kKey) {
        ProviderInstance instance;
        instance.name = QStringLiteral("Google Gemini");
        instance.type = QStringLiteral("gemini");
        instance.address = QString::fromLatin1(api.origin());
        instance.key = QString::fromLatin1(key);
        instance.settings = settings;
        return std::make_unique<GeminiProvider>(instance);
    }

    JobRequest request(const QString &capability, const QJsonObject &params, const QJsonObject &inputs = {}, const QString &model = QStringLiteral("gemini-3.1-flash-image")) {
        JobRequest job;
        job.capability = capability;
        job.model = QJsonObject{{"resource", "fugue/image-model"}, {"name", model}};
        job.params = params;
        job.inputs = inputs;
        job.outputDir = dir.filePath("out");
        QDir().mkpath(job.outputDir);
        return job;
    }

    QJsonObject picture(const QString &name) {
        QFile file(dir.filePath(name));
        if (file.open(QIODevice::WriteOnly))
            file.write(kPng);
        return QJsonObject{{"path", dir.filePath(name)}, {"format", "png"}};
    }

    QJsonObject lastBody() const {
        const auto posts = api.requestsTo("POST", kPath);
        return posts.isEmpty() ? QJsonObject{} : parse(posts.last().body);
    }

    QTemporaryDir dir;
    FakeHttpServer api;
    HttpResponse reply;
};

JobStatus run(GeminiProvider &provider, const JobRequest &request, JobResult *result = nullptr) {
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

class TestGeminiProvider : public QObject {
    Q_OBJECT

private slots:
    void theShippedTableOffersEveryModelForGenerateAndEdit() {
        Fixture fixture;
        auto provider = fixture.provider({{"image_models", "my-model, gemini-3-pro-image"}});
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY2(described.ok(), qPrintable(described.ok() ? QString() : described.error().message));
        QCOMPARE(described->engine, QStringLiteral("gemini@127.0.0.1"));
        for (const char *capability : {"image.generate", "image.edit"}) {
            QStringList names;
            for (const OfferedModel &model : described->offer(capability)->models)
                names.append(model.handle.value("name").toString());
            QCOMPARE(names, (QStringList{"gemini-nano-banana-2.1", "gemini-3.1-flash-image", "gemini-3.1-flash-lite-image", "gemini-3-pro-image", "gemini-2.5-flash-image", "my-model"}));
        }
        const OfferedModel flash = described->offer("image.generate")->models.at(1);
        QCOMPARE(flash.status, ModelStatus::Ready);
        QVERIFY(flash.title.contains("$0.067/1K image"));
        QCOMPARE(flash.handle, (QJsonObject{{"resource", "fugue/image-model"}, {"name", "gemini-3.1-flash-image"}}));

        // Without a key nothing can run, and nothing is sent.
        auto keyless = fixture.provider({}, "");
        const auto bare = await<Description>([&](auto reply) { keyless->describe(reply); });
        QCOMPARE(bare->offer("image.generate")->models.first().status, ModelStatus::Unavailable);
        QCOMPARE(run(*keyless, fixture.request("image.generate", {{"prompt", "x"}})).error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(fixture.api.requests().isEmpty());
    }

    void aGenerateIsOneCallWithTheKeyOnlyInTheHeader() {
        Fixture fixture;
        // Gemini 3 Pro shows interim pictures as thoughts: the result is the first one that is not.
        fixture.reply = answer({imagePart("thought-picture", true), QJsonObject{{"text", "ok"}}, imagePart(kPng)});
        auto provider = fixture.provider();
        JobResult result;
        const JobStatus status = run(*provider, fixture.request("image.generate", {{"prompt", "a dot"}, {"width", 1920}, {"height", 1080}, {"seed", 5000000000.0}}), &result);
        QCOMPARE(status.state, QStringLiteral("done"));

        const QJsonObject body = fixture.lastBody();
        const QJsonArray parts = body.value("contents").toArray().first().toObject().value("parts").toArray();
        QCOMPARE(parts.size(), 1);
        QCOMPARE(parts.first().toObject().value("text").toString(), QStringLiteral("a dot"));
        const QJsonObject config = body.value("generationConfig").toObject();
        QCOMPARE(config.value("responseModalities").toArray(), (QJsonArray{"TEXT", "IMAGE"}));
        QCOMPARE(config.value("imageConfig").toObject().value("aspectRatio").toString(), QStringLiteral("16:9"));
        QCOMPARE(config.value("imageConfig").toObject().value("imageSize").toString(), QStringLiteral("2K"));
        QCOMPARE(config.value("seed").toInt(), 705032704); // 5e9 modulo 2^31: Google's seed is an int32

        QCOMPARE(readAll(result.outputs.value("image").toObject().value("path").toString()), kPng);
        QCOMPARE(result.used.value("engine").toString(), QStringLiteral("gemini@gemini-3.1-flash-image"));
        QCOMPARE(result.used.value("seed").toInt(), 705032704); // what was sent
        QCOMPARE(result.used.value("width").toInt(), 1); // the picture's own size

        // The key is in the header of the one call, and in no URL, body, result or error.
        const QList<HttpRequest> requests = fixture.api.requests();
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.first().headers.value("x-goog-api-key"), kKey);
        QVERIFY(!requests.first().path.contains("key="));
        QVERIFY(!requests.first().path.contains(kKey));
        QVERIFY(!requests.first().body.contains(kKey));
        QVERIFY(!json(result.used).contains(kKey));
    }

    void anEditSendsItsPicturesInlineAndAMaskIsRefused() {
        Fixture fixture;
        auto provider = fixture.provider();
        const QJsonObject image = fixture.picture("start.png");
        JobRequest edit = fixture.request("image.edit", {{"prompt", "make it night"}}, {{"images", QJsonArray{image, image}}});
        QCOMPARE(run(*provider, edit).state, QStringLiteral("done"));
        const QJsonArray parts = fixture.lastBody().value("contents").toArray().first().toObject().value("parts").toArray();
        QCOMPARE(parts.size(), 3); // the prompt, then each picture
        QCOMPARE(parts.at(0).toObject().value("text").toString(), QStringLiteral("make it night"));
        const QJsonObject inlined = parts.at(1).toObject().value("inline_data").toObject();
        QCOMPARE(inlined.value("mime_type").toString(), QStringLiteral("image/png"));
        QCOMPARE(QByteArray::fromBase64(inlined.value("data").toString().toLatin1()), kPng);
        QVERIFY(!fixture.lastBody().value("generationConfig").toObject().contains("imageConfig")); // no size asked: the model keeps its own

        const JobStatus masked = run(*provider, fixture.request("image.edit", {{"prompt", "x"}}, {{"image", image}, {"mask", image}}));
        QCOMPARE(masked.error.code, QStringLiteral("mask_unsupported"));
        QCOMPARE(run(*provider, fixture.request("image.edit", {{"prompt", "x"}})).error.code, QStringLiteral("missing_input"));
        QCOMPARE(fixture.api.requestsTo("POST", kPath).size(), 1); // the refusals sent nothing
    }

    void aSafetyBlockOrATextOnlyAnswerIsAClearError() {
        Fixture fixture;
        auto provider = fixture.provider();
        const JobRequest job = fixture.request("image.generate", {{"prompt", "x"}});

        fixture.reply = HttpResponse{200, json({{"promptFeedback", QJsonObject{{"blockReason", "PROHIBITED_CONTENT"}}}})};
        JobStatus status = run(*provider, job);
        QCOMPARE(status.error.code, QStringLiteral("node_failed"));
        QVERIFY(status.error.message.contains("safety rules") && status.error.message.contains("PROHIBITED_CONTENT"));

        fixture.reply = answer({}, "IMAGE_SAFETY");
        status = run(*provider, job);
        QVERIFY(status.error.message.contains("safety rules") && status.error.message.contains("IMAGE_SAFETY"));

        fixture.reply = answer({QJsonObject{{"text", "I can only describe that picture."}}});
        status = run(*provider, job);
        QCOMPARE(status.error.code, QStringLiteral("node_failed"));
        QVERIFY(status.error.message.contains("made no picture") && status.error.message.contains("I can only describe"));

        fixture.reply = answer({imagePart("not a picture")});
        QVERIFY(run(*provider, job).error.message.contains("not a picture"));
    }

    void errorsMapToTheSpecsCodesAndNeverHoldTheKey() {
        Fixture fixture;
        const JobRequest job = fixture.request("image.generate", {{"prompt", "x"}});
        const auto error = [](int code, const QString &status, const QString &message) {
            return HttpResponse{code, json({{"error", QJsonObject{{"code", code}, {"message", message}, {"status", status}}}})};
        };

        // A wrong key: Google answers 400, and may repeat what it got.
        auto wrong = fixture.provider({}, "AIzaWrongKey0123456789abcdefghijklmnopq");
        JobStatus status = run(*wrong, job);
        QCOMPARE(status.error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(status.error.message.contains("refused the key"));
        QVERIFY(status.error.hint.contains("Settings > Providers"));
        QVERIFY(!json(status.error.toJson()).contains("AIzaWrongKey"));

        auto provider = fixture.provider();
        fixture.reply = error(403, "PERMISSION_DENIED", "Key " + QString::fromLatin1(kKey) + " is blocked");
        status = run(*provider, job);
        QCOMPARE(status.error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(!json(status.error.toJson()).contains(kKey));
        QVERIFY(status.error.message.contains("***"));

        fixture.reply = error(429, "RESOURCE_EXHAUSTED", "You exceeded your current quota");
        fixture.reply.headers = {"Retry-After: 30"};
        status = run(*provider, job);
        QCOMPARE(status.error.code, QStringLiteral("node_failed"));
        QVERIFY(status.error.message.contains("429") && status.error.message.contains("30 seconds") && status.error.message.contains("exceeded your current quota"));
        QVERIFY(status.error.hint.contains("no free quota"));

        fixture.reply = error(400, "FAILED_PRECONDITION", "User location is not supported");
        QVERIFY(run(*provider, job).error.hint.contains("billing"));
        fixture.reply = error(400, "INVALID_ARGUMENT", "Image size is not supported");
        status = run(*provider, job);
        QCOMPARE(status.error.code, QStringLiteral("node_failed"));
        QVERIFY(status.error.message.contains("Image size is not supported"));
        fixture.reply = error(503, "UNAVAILABLE", "The model is overloaded");
        QCOMPARE(run(*provider, job).error.code, QStringLiteral("provider_unreachable"));
        fixture.reply = error(500, "INTERNAL", "boom");
        QCOMPARE(run(*provider, job).error.code, QStringLiteral("node_failed"));
        fixture.reply = error(404, "NOT_FOUND", "models/nope is not found");
        QCOMPARE(run(*provider, fixture.request("image.generate", {{"prompt", "x"}}, {}, "gemini-3.1-flash-image")).error.code, QStringLiteral("model_missing"));
        // A redirect is an error, and the key goes nowhere else.
        FakeHttpServer elsewhere;
        elsewhere.listen();
        fixture.reply = HttpResponse{302, {}, "text/plain", false, -1, {"Location: " + elsewhere.origin() + "/elsewhere"}};
        QCOMPARE(run(*provider, job).error.code, QStringLiteral("provider_unreachable"));
        QVERIFY2(elsewhere.requests().isEmpty(), "the redirect was followed");
    }

    void aJobHasOneDeadlineAndACancelAbortsIt() {
        Fixture fixture;
        fixture.reply = HttpResponse{200, {}, "application/json", true};
        auto slow = fixture.provider({{"timeout_s", 0.2}});
        const JobStatus late = run(*slow, fixture.request("image.generate", {{"prompt", "x"}}));
        QCOMPARE(late.error.code, QStringLiteral("node_timeout"));
        QVERIFY(late.error.hint.contains("timeout_s"));

        auto provider = fixture.provider();
        const auto submitted = await<QString>([&](auto reply) { provider->submit(fixture.request("image.generate", {{"prompt", "x"}}), reply); });
        QVERIFY(submitted.ok());
        QTRY_COMPARE(fixture.api.requests().size(), 1);
        const auto cancelled = await<QString>([&](auto reply) { provider->cancel(submitted.value(), reply); });
        QCOMPARE(cancelled.value(), QStringLiteral("cancelled"));
        QCOMPARE(await<JobStatus>([&](auto reply) { provider->status(submitted.value(), reply); })->state, QStringLiteral("cancelled"));
    }

    void theKeyIsNeverSentOverPlainHttpOffMachine() {
        ProviderInstance instance;
        instance.name = QStringLiteral("Google Gemini");
        instance.type = QStringLiteral("gemini");
        instance.address = QStringLiteral("http://example.com");
        instance.key = QString::fromLatin1(kKey);
        GeminiProvider provider(instance);
        const auto described = await<Description>([&](auto reply) { provider.describe(reply); });
        QVERIFY(!described.ok());
        QCOMPARE(described.error().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.error().hint.contains("https://"));
    }

    void aGeminiInstanceIsMadeByTheRegistry() {
        ProviderInstance instance;
        instance.name = QStringLiteral("Google Gemini");
        instance.type = QStringLiteral("gemini");
        instance.key = "k";
        const auto provider = createProvider(instance, QDir::tempPath() + "/no-such-modules");
        QVERIFY(provider.ok());
        QVERIFY(dynamic_cast<GeminiProvider *>(provider.value().get()));
        QCOMPARE(geminiShippedModels().size(), 5);
        QCOMPARE(geminiAddress(instance), QStringLiteral("https://generativelanguage.googleapis.com"));
    }
};

FUGUE_TEST_MAIN(TestGeminiProvider, QCoreApplication)
#include "test_gemini_provider.moc"
