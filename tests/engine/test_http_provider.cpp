// The generic `http` provider (spec 8.7, #401): an OpenAI-compatible endpoint (images, edits, chat) and the
// stable-diffusion.cpp native server, against a fake server on the loopback. Every key here is made up.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/http_provider.h"
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

const QByteArray kKey = "sk-test-0123456789abcdef";

// A 1x1 PNG.
const QByteArray kPng = QByteArray::fromBase64(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");

QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

HttpResponse needsKey(const HttpRequest &request, const HttpResponse &fine) {
    if (request.headers.value("authorization") != "Bearer " + kKey)
        return HttpResponse{401, json({{"error", QJsonObject{{"message", "Incorrect API key provided: " + QString::fromLatin1(kKey)}}}})};
    return fine;
}

QByteArray modelList(const QStringList &ids) {
    QJsonArray data;
    for (const QString &id : ids)
        data.append(QJsonObject{{"id", id}, {"object", "model"}});
    return json({{"object", "list"}, {"data", data}});
}

QByteArray picture(const QByteArray &png = kPng) {
    return json({{"created", 1}, {"data", QJsonArray{QJsonObject{{"b64_json", QString::fromLatin1(png.toBase64())}}}}});
}

QByteArray streamed(const QStringList &pieces) {
    QByteArray body;
    for (const QString &piece : pieces)
        body += "data: " + json({{"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{{"content", piece}}}}}}}) + "\n\n";
    body += "data: " + json({{"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{}}, {"finish_reason", "stop"}}}}}) + "\n\n";
    return body + "data: [DONE]\n\n";
}

class Fixture {
public:
    Fixture() { server.listen(); }

    QString address() const { return QString::fromLatin1(server.origin()) + "/v1"; }

    std::unique_ptr<HttpProvider> provider(const QJsonObject &settings = {}, const QByteArray &key = kKey) {
        ProviderInstance instance;
        instance.name = QStringLiteral("My endpoint");
        instance.type = QStringLiteral("http");
        instance.address = address();
        instance.key = QString::fromLatin1(key);
        instance.settings = settings;
        instance.settings.insert("poll_ms", 10);
        return std::make_unique<HttpProvider>(instance);
    }

    // A service that lists these models and takes the key.
    void service(const QStringList &models) {
        server.on("GET", "/v1/models", [models](const HttpRequest &request) { return needsKey(request, HttpResponse{200, modelList(models)}); });
    }

    JobRequest request(const QString &capability, const QJsonObject &params, const QJsonObject &inputs = {}) {
        JobRequest job;
        job.capability = capability;
        job.params = params;
        job.inputs = inputs;
        job.outputDir = dir.filePath("out");
        QDir().mkpath(job.outputDir);
        return job;
    }

    QString writeInput(const QString &name, const QByteArray &bytes) {
        const QString path = dir.filePath(name);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
            file.write(bytes);
        return path;
    }

    QTemporaryDir dir;
    FakeHttpServer server;
};

// Submits and waits for the end. The submit's own error, or the job's, is returned as the status.
JobStatus run(HttpProvider &provider, const JobRequest &request, JobResult *result = nullptr, QString *jobId = nullptr) {
    const auto submitted = await<QString>([&](auto reply) { provider.submit(request, reply); });
    if (!submitted.ok()) {
        JobStatus status;
        status.state = QStringLiteral("error");
        status.error = submitted.error();
        return status;
    }
    if (jobId)
        *jobId = submitted.value();
    JobStatus status;
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

QJsonObject lastBody(const FakeHttpServer &server, const QByteArray &path) {
    const auto posts = server.requestsTo("POST", path);
    return posts.isEmpty() ? QJsonObject{} : QJsonDocument::fromJson(posts.last().body).object();
}

const QJsonObject kDraw{{"resource", "fugue/image-model"}, {"name", "gpt-image-1"}};

} // namespace

class TestHttpProvider : public QObject {
    Q_OBJECT

private slots:
    // --- OpenAI-compatible: the model list ---

    void modelsAreAdvertisedAsHandlesAndSortedByWhatTheyDo() {
        Fixture fixture;
        fixture.service({"gpt-image-1", "gpt-4o-mini", "text-embedding-3-small", "dall-e-3"});
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY2(described.ok(), qPrintable(described.ok() ? QString() : described.error().message));
        QCOMPARE(described->engine, QStringLiteral("openai@127.0.0.1"));
        const CapabilityOffer *images = described->offer("image.generate");
        QVERIFY(images);
        QCOMPARE(images->models.size(), 2);
        QCOMPARE(images->models.first().handle, kDraw);
        QCOMPARE(images->models.first().status, ModelStatus::Ready);
        QCOMPARE(described->offer("image.edit")->models.size(), 2);
        const CapabilityOffer *text = described->offer("text.generate");
        QVERIFY(text);
        QCOMPARE(text->models.size(), 1); // the embedding model is no chat model
        QCOMPARE(text->models.first().handle, (QJsonObject{{"resource", "fugue/text-model"}, {"name", "gpt-4o-mini"}}));
    }

    void theSettingsNameTheModelsWhenTheServiceCannot() {
        Fixture fixture; // no /models at all
        auto provider = fixture.provider({{"image_models", "my-flux, other"}, {"text_models", QJsonArray{"my-llm"}}});
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY2(described.ok(), qPrintable(described.ok() ? QString() : described.error().message));
        QCOMPARE(described->offer("image.generate")->models.size(), 2);
        QCOMPARE(described->offer("text.generate")->models.first().handle.value("name").toString(), QStringLiteral("my-llm"));
    }

    // --- OpenAI-compatible: Generate Image and Describe Image through one instance ---

    void oneInstanceGeneratesAPictureAndDescribesIt() {
        Fixture fixture;
        fixture.service({"gpt-image-1", "gpt-4o-mini"});
        fixture.server.on("POST", "/v1/images/generations", [](const HttpRequest &request) { return needsKey(request, HttpResponse{200, picture()}); });
        fixture.server.on("POST", "/v1/chat/completions", [](const HttpRequest &request) {
            return needsKey(request, HttpResponse{200, streamed({"A single ", "pixel."}), "text/event-stream"});
        });
        auto provider = fixture.provider();
        QVERIFY(await<Description>([&](auto reply) { provider->describe(reply); }).ok());

        JobResult drawn;
        const JobStatus drew = run(*provider, [&] {
            JobRequest request = fixture.request("image.generate", {{"prompt", "a dot"}, {"width", 1024}, {"height", 1024}, {"seed", 5}, {"steps", 20}});
            request.model = kDraw;
            return request;
        }(), &drawn);
        QCOMPARE(drew.state, QStringLiteral("done"));
        const QJsonObject body = lastBody(fixture.server, "/v1/images/generations");
        QCOMPARE(body.value("model").toString(), QStringLiteral("gpt-image-1"));
        QCOMPARE(body.value("prompt").toString(), QStringLiteral("a dot"));
        QCOMPARE(body.value("size").toString(), QStringLiteral("1024x1024"));
        QCOMPARE(body.value("n").toInt(), 1);
        QVERIFY(!body.contains("seed")); // an OpenAI service has none, so none is sent or claimed
        QVERIFY(!drawn.used.contains("seed"));
        const QString image = drawn.outputs.value("image").toObject().value("path").toString();
        QCOMPARE(readAll(image), kPng);
        QCOMPARE(drawn.used.value("width").toInt(), 1); // the picture's own size
        QCOMPARE(drawn.used.value("model").toObject(), kDraw);
        QCOMPARE(fixture.server.requestsTo("POST", "/v1/images/generations").first().headers.value("authorization"), "Bearer " + kKey);

        JobResult described;
        QJsonObject seen{{"path", image}, {"format", "png"}};
        const JobStatus read = run(*provider, [&] {
            JobRequest request = fixture.request("text.generate", {{"prompt", "What is this?"}}, {{"image", seen}});
            request.model = QJsonObject{{"resource", "fugue/text-model"}, {"name", "gpt-4o-mini"}};
            return request;
        }(), &described);
        QCOMPARE(read.state, QStringLiteral("done"));
        QCOMPARE(described.outputs.value("text").toString(), QStringLiteral("A single pixel."));
        const QJsonArray messages = lastBody(fixture.server, "/v1/chat/completions").value("messages").toArray();
        const QJsonObject content = messages.last().toObject().value("content").toArray().last().toObject();
        QVERIFY(content.value("image_url").toObject().value("url").toString().startsWith("data:image/png;base64,"));
    }

    void aPictureNamedByUrlIsFetchedWithoutTheKey() {
        Fixture fixture;
        fixture.server.on("POST", "/v1/images/generations", [&fixture](const HttpRequest &) {
            return HttpResponse{200, json({{"data", QJsonArray{QJsonObject{{"url", QString::fromLatin1(fixture.server.origin()) + "/files/1.png"}}}}})};
        });
        fixture.server.on("GET", "/files/1.png", [](const HttpRequest &) { return HttpResponse{200, kPng, "image/png"}; });
        auto provider = fixture.provider({{"image_models", "dall-e-3"}});
        QVERIFY(await<Description>([&](auto reply) { provider->describe(reply); }).ok());
        JobRequest request = fixture.request("image.generate", {{"prompt", "a dot"}});
        request.model = QJsonObject{{"resource", "fugue/image-model"}, {"name", "dall-e-3"}};
        JobResult result;
        QCOMPARE(run(*provider, request, &result).state, QStringLiteral("done"));
        QVERIFY(result.outputs.contains("image"));
        QVERIFY(fixture.server.requestsTo("GET", "/files/1.png").first().headers.value("authorization").isEmpty());
    }

    void anEditSendsItsPicturesAsAMultipartForm() {
        Fixture fixture;
        fixture.service({"gpt-image-1"});
        fixture.server.on("POST", "/v1/images/edits", [](const HttpRequest &request) { return needsKey(request, HttpResponse{200, picture()}); });
        auto provider = fixture.provider();
        QVERIFY(await<Description>([&](auto reply) { provider->describe(reply); }).ok());
        const QJsonObject one{{"path", fixture.writeInput("a.png", kPng)}, {"format", "png"}};
        const QJsonObject two{{"path", fixture.writeInput("b.png", kPng)}, {"format", "png"}};
        JobRequest request = fixture.request("image.edit", {{"prompt", "make it blue"}}, {{"images", QJsonArray{one, two}}});
        request.model = kDraw;
        JobResult result;
        QCOMPARE(run(*provider, request, &result).state, QStringLiteral("done"));
        const HttpRequest sent = fixture.server.requestsTo("POST", "/v1/images/edits").first();
        QVERIFY(sent.headers.value("content-type").startsWith("multipart/form-data; boundary="));
        QCOMPARE(sent.body.count("name=\"image[]\""), 2);
        QVERIFY(sent.body.contains("name=\"prompt\"\r\n\r\nmake it blue"));
        QVERIFY(sent.body.contains("name=\"model\"\r\n\r\ngpt-image-1"));
        QVERIFY(QFile::exists(result.outputs.value("image").toObject().value("path").toString()));
    }

    void aMaskIsRefusedBeforeAnythingIsSent() {
        Fixture fixture;
        fixture.service({"gpt-image-1"});
        auto provider = fixture.provider();
        QVERIFY(await<Description>([&](auto reply) { provider->describe(reply); }).ok());
        const QJsonObject image{{"path", fixture.writeInput("a.png", kPng)}, {"format", "png"}};
        JobRequest request = fixture.request("image.generate", {{"prompt", "x"}}, {{"image", image}, {"mask", image}});
        request.model = kDraw;
        const JobStatus status = run(*provider, request);
        QCOMPARE(status.error.code, QStringLiteral("mask_unsupported"));
        QVERIFY(fixture.server.requestsTo("POST", "/v1/images/generations").isEmpty());
        QVERIFY(fixture.server.requestsTo("POST", "/v1/images/edits").isEmpty());
    }

    // --- errors (8.8, 13.3) ---

    void errorsMapToTheSpecsCodesWithAHint_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("header");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("message");
        const QByteArray plain = json({{"error", QJsonObject{{"message", "no good"}}}});
        QTest::newRow("401") << 401 << plain << QByteArray() << "provider_unreachable" << "refused the key";
        QTest::newRow("403") << 403 << plain << QByteArray() << "provider_unreachable" << "refused the key";
        QTest::newRow("429 with Retry-After") << 429 << plain << QByteArray("Retry-After: 17") << "node_failed" << "wait 17 seconds";
        QTest::newRow("429 without") << 429 << plain << QByteArray() << "node_failed" << "limiting the rate";
        QTest::newRow("429 out of quota") << 429 << json({{"error", QJsonObject{{"message", "billing"}, {"code", "insufficient_quota"}}}})
                                          << QByteArray() << "node_failed" << "no quota left";
        QTest::newRow("500") << 500 << plain << QByteArray() << "node_failed" << "had an error (HTTP 500): no good";
        QTest::newRow("502") << 502 << QByteArray("<html>") << QByteArray() << "provider_unreachable" << "not available right now";
        QTest::newRow("503 loading") << 503 << json({{"error", QJsonObject{{"message", "Model is loading"}}}}) << QByteArray()
                                     << "model_not_running" << "loading";
        QTest::newRow("404 model") << 404 << json({{"error", QJsonObject{{"message", "The model `x` does not exist"}, {"code", "model_not_found"}}}})
                                   << QByteArray() << "model_missing" << "no such model";
        QTest::newRow("404 path") << 404 << QByteArray("{}") << QByteArray() << "node_failed" << "nothing at";
        QTest::newRow("400") << 400 << json({{"error", QJsonObject{{"message", "Invalid size"}}}}) << QByteArray() << "node_failed" << "Invalid size";
        QTest::newRow("out of memory") << 500 << json({{"error", QJsonObject{{"message", "CUDA out of memory"}}}}) << QByteArray() << "out_of_memory" << "out of memory";
    }

    void errorsMapToTheSpecsCodesWithAHint() {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(QByteArray, header);
        QFETCH(QString, code);
        QFETCH(QString, message);
        Fixture fixture;
        fixture.server.on("POST", "/v1/images/generations", [&](const HttpRequest &) {
            HttpResponse response{status, body};
            if (!header.isEmpty())
                response.headers.append(header);
            return response;
        });
        auto provider = fixture.provider({{"image_models", "gpt-image-1"}});
        JobRequest request = fixture.request("image.generate", {{"prompt", "x"}});
        request.model = kDraw;
        const JobStatus ended = run(*provider, request);
        QCOMPARE(ended.state, QStringLiteral("error"));
        QCOMPARE(ended.error.code, code);
        QVERIFY2(ended.error.message.contains(message), qPrintable(ended.error.message));
        QVERIFY2(!ended.error.hint.isEmpty(), qPrintable(ended.error.message));
    }

    void aSlowServiceTimesOutAsUnreachable() {
        Fixture fixture;
        fixture.server.on("POST", "/v1/images/generations", [](const HttpRequest &) {
            HttpResponse response;
            response.hold = true;
            return response;
        });
        auto provider = fixture.provider({{"image_models", "gpt-image-1"}, {"timeout_s", 0.3}});
        JobRequest request = fixture.request("image.generate", {{"prompt", "x"}});
        request.model = kDraw;
        const JobStatus ended = run(*provider, request);
        QCOMPARE(ended.error.code, QStringLiteral("provider_unreachable"));
        QVERIFY2(ended.error.message.contains("said nothing"), qPrintable(ended.error.message));
        QVERIFY(ended.error.hint.contains("timeout_s"));
    }

    void anAnswerThatIsNotJsonIsSaid() {
        Fixture fixture;
        fixture.server.on("POST", "/v1/images/generations", [](const HttpRequest &) { return HttpResponse{200, "<html>hello</html>", "text/html"}; });
        auto provider = fixture.provider({{"image_models", "gpt-image-1"}});
        JobRequest request = fixture.request("image.generate", {{"prompt", "x"}});
        request.model = kDraw;
        const JobStatus ended = run(*provider, request);
        QCOMPARE(ended.error.code, QStringLiteral("node_failed"));
        QVERIFY2(ended.error.message.contains("not JSON"), qPrintable(ended.error.message));
        QVERIFY(!ended.error.hint.isEmpty());
    }

    void aServiceThatIsDownIsUnreachable() {
        Fixture fixture;
        auto provider = fixture.provider();
        fixture.server.close();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!described.ok());
        QCOMPARE(described.error().code, QStringLiteral("provider_unreachable"));
        QVERIFY(!described.error().hint.isEmpty());
    }

    void aRefusedKeyIsUnreachableWithAHintAndNeverEchoesTheKey() {
        Fixture fixture;
        fixture.service({"gpt-image-1"});
        auto provider = fixture.provider({}, "sk-wrong-key-123");
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!described.ok());
        QCOMPARE(described.error().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.error().hint.contains("providers.json"));
        // The fake server's 401 repeats the key it was given (as services do): it must not reach a message.
        fixture.server.on("POST", "/v1/images/generations", [](const HttpRequest &request) {
            return HttpResponse{401, json({{"error", QJsonObject{{"message", "Incorrect API key: " + QString::fromLatin1(request.headers.value("authorization").mid(7))}}}})};
        });
        JobRequest request = fixture.request("image.generate", {{"prompt", "x"}});
        request.model = kDraw;
        const JobStatus ended = run(*provider, request);
        const QByteArray everything = QJsonDocument(ended.error.toJson()).toJson();
        QVERIFY(!everything.contains("sk-wrong-key-123"));
        const auto refused = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!QJsonDocument(refused.error().toJson()).toJson().contains("sk-wrong-key-123"));
    }

    void anAddressThatIsNotAnHttpUrlIsSaid() {
        ProviderInstance instance;
        instance.name = QStringLiteral("My endpoint");
        instance.type = QStringLiteral("http");
        instance.address = QStringLiteral("ftp://example.com");
        HttpProvider provider(instance);
        const auto described = await<Description>([&](auto reply) { provider.describe(reply); });
        QCOMPARE(described.error().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.error().message.contains("no usable address"));
    }

    void aCancelledJobEndsCancelled() {
        Fixture fixture;
        fixture.server.on("POST", "/v1/images/generations", [](const HttpRequest &) {
            HttpResponse response;
            response.hold = true;
            return response;
        });
        auto provider = fixture.provider({{"image_models", "gpt-image-1"}});
        JobRequest request = fixture.request("image.generate", {{"prompt", "x"}});
        request.model = kDraw;
        const QString id = await<QString>([&](auto reply) { provider->submit(request, reply); }).value();
        QCOMPARE(await<QString>([&](auto reply) { provider->cancel(id, reply); }).value(), QStringLiteral("cancelled"));
        QCOMPARE(await<JobStatus>([&](auto reply) { provider->status(id, reply); }).value().state, QStringLiteral("cancelled"));
    }

    // --- picture links, size caps, retries (review of #401) ---

    void aPictureLinkIsOnlyFetchedWhenItIsAPlaceAServiceMayName() {
        QVERIFY(downloadUrlAllowed(QUrl("https://cdn.example.com/a.png"), "api.example.com"));
        QVERIFY(!downloadUrlAllowed(QUrl("file:///mnt/windows/models/huge.safetensors"), "api.example.com"));
        QVERIFY(!downloadUrlAllowed(QUrl("data:image/png;base64,AAAA"), "127.0.0.1"));
        QVERIFY(!downloadUrlAllowed(QUrl("qrc:/x"), "127.0.0.1"));
        QVERIFY(!downloadUrlAllowed(QUrl("http://127.0.0.1:8080/admin"), "api.example.com"));
        QVERIFY(!downloadUrlAllowed(QUrl("http://localhost/x"), "api.example.com"));
        QVERIFY(!downloadUrlAllowed(QUrl("http://169.254.169.254/latest"), "api.example.com"));
        QVERIFY(!downloadUrlAllowed(QUrl("http://192.168.1.5/x"), "api.example.com"));
        QVERIFY(!downloadUrlAllowed(QUrl("http://[::1]/x"), "api.example.com"));
        QVERIFY(downloadUrlAllowed(QUrl("http://127.0.0.1:8080/x"), "127.0.0.1")); // the service is local itself
        QVERIFY(downloadUrlAllowed(QUrl("http://192.168.1.5/x"), "192.168.1.9"));
    }

    void aFileOrDataLinkIsRefusedAndARelativeLinkWorks() {
        Fixture fixture;
        auto link = std::make_shared<QString>();
        fixture.server.on("POST", "/v1/images/generations", [link](const HttpRequest &) {
            return HttpResponse{200, json({{"data", QJsonArray{QJsonObject{{"url", *link}}}}})};
        });
        fixture.server.on("GET", "/v1/pic.png", [](const HttpRequest &) { return HttpResponse{200, kPng, "image/png"}; });
        auto provider = fixture.provider({{"image_models", "dall-e-3"}});
        JobRequest request = fixture.request("image.generate", {{"prompt", "a dot"}});
        request.model = QJsonObject{{"resource", "fugue/image-model"}, {"name", "dall-e-3"}};
        for (const QString &bad : {QStringLiteral("file:///etc/hostname"), QStringLiteral("data:image/png;base64,AAAA")}) {
            *link = bad;
            const JobStatus ended = run(*provider, request);
            QCOMPARE(ended.error.code, QStringLiteral("node_failed"));
            QVERIFY2(ended.error.message.contains("does not fetch"), qPrintable(ended.error.message));
        }
        *link = QStringLiteral("pic.png"); // relative to the address
        JobResult result;
        QCOMPARE(run(*provider, request, &result).state, QStringLiteral("done"));
        QCOMPARE(readAll(result.outputs.value("image").toObject().value("path").toString()), kPng);
    }

    void aRedirectOfAPictureLinkToAFileIsRefusedAndAnExpiredLinkIsSaid() {
        Fixture fixture;
        auto target = std::make_shared<HttpResponse>();
        fixture.server.on("POST", "/v1/images/generations", [&fixture](const HttpRequest &) {
            return HttpResponse{200, json({{"data", QJsonArray{QJsonObject{{"url", QString::fromLatin1(fixture.server.origin()) + "/files/1.png"}}}}})};
        });
        fixture.server.on("GET", "/files/1.png", [target](const HttpRequest &) { return *target; });
        auto provider = fixture.provider({{"image_models", "dall-e-3"}});
        JobRequest request = fixture.request("image.generate", {{"prompt", "a dot"}});
        request.model = QJsonObject{{"resource", "fugue/image-model"}, {"name", "dall-e-3"}};
        *target = HttpResponse{302, "", "text/plain", false, -1, {"Location: file:///etc/hostname"}};
        const JobStatus redirected = run(*provider, request);
        QVERIFY2(redirected.error.message.contains("sent the picture link on"), qPrintable(redirected.error.message));
        *target = HttpResponse{403, "<Error>Expired</Error>", "text/xml"};
        const JobStatus expired = run(*provider, request);
        QVERIFY2(expired.error.message.contains("picture link was refused"), qPrintable(expired.error.message));
        QVERIFY(!expired.error.message.contains("key"));
    }

    void anAnswerPastTheSizeCapIsCutOff() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &) { return HttpResponse{200, QByteArray(70 << 20, 'x')}; });
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!described.ok());
        QVERIFY2(described.error().message.contains("more data"), qPrintable(described.error().message));
        QVERIFY(!described.error().hint.isEmpty());
    }

    void aRedirectedAddressNamesTheNewOne() {
        Fixture fixture;
        fixture.server.on("GET", "/v1/models", [](const HttpRequest &) {
            return HttpResponse{301, "", "text/plain", false, -1, {"Location: https://api.example.com/v1/models"}};
        });
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY(!described.ok());
        QVERIFY2(described.error().message.contains("https://api.example.com/v1/models"), qPrintable(described.error().message));
        QVERIFY(described.error().hint.contains("Use that address"));
    }

    void anImageThatIsNotAPictureAndAnEmptyDataListFail() {
        Fixture fixture;
        auto body = std::make_shared<QByteArray>();
        fixture.server.on("POST", "/v1/images/generations", [body](const HttpRequest &) { return HttpResponse{200, *body}; });
        auto provider = fixture.provider({{"image_models", "gpt-image-1"}});
        JobRequest request = fixture.request("image.generate", {{"prompt", "x"}});
        request.model = kDraw;
        *body = json({{"data", QJsonArray{QJsonObject{{"b64_json", QString::fromLatin1(QByteArray("just text").toBase64())}}}}});
        QVERIFY(run(*provider, request).error.message.contains("not a picture"));
        *body = json({{"data", QJsonArray{}}});
        QVERIFY(run(*provider, request).error.message.contains("without an image"));
    }

    void aNativePollRetriesATransientAnswerAndScrubsTheServersText() {
        Fixture fixture;
        fixture.server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) { return HttpResponse{200, json({{"model", QJsonObject{{"name", "m"}}}})}; });
        fixture.server.on("POST", "/sdcpp/v1/img_gen", [](const HttpRequest &) { return HttpResponse{202, json({{"id", "j"}, {"status", "queued"}})}; });
        auto polls = std::make_shared<int>(0);
        fixture.server.on("GET", "/sdcpp/v1/jobs/j", [polls](const HttpRequest &) {
            if (++*polls <= 2)
                return HttpResponse{(*polls == 1 ? 429 : 503), "{}"};
            return HttpResponse{200, json({{"id", "j"}, {"status", "failed"}, {"error", QJsonObject{{"message", "bad key " + QString::fromLatin1(kKey)}}}})};
        });
        auto provider = fixture.provider();
        JobRequest request = fixture.request("image.generate", {{"prompt", "x"}});
        request.model = QJsonObject{{"resource", "fugue/image-model"}, {"name", "m"}};
        const JobStatus ended = run(*provider, request);
        QVERIFY(*polls >= 3); // it kept asking
        QVERIFY2(ended.error.message.contains("bad key"), qPrintable(ended.error.message));
        QVERIFY(!ended.error.message.contains(QString::fromLatin1(kKey)));
    }

    void aNativeJobThatNeverEndsGivesUp() {
        Fixture fixture;
        fixture.server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) { return HttpResponse{200, json({{"model", QJsonObject{{"name", "m"}}}})}; });
        fixture.server.on("POST", "/sdcpp/v1/img_gen", [](const HttpRequest &) { return HttpResponse{202, json({{"id", "j"}, {"status", "queued"}})}; });
        fixture.server.on("GET", "/sdcpp/v1/jobs/j", [](const HttpRequest &) { return HttpResponse{200, json({{"id", "j"}, {"status", "generating"}})}; });
        auto provider = fixture.provider({{"timeout_s", 0.2}});
        JobRequest request = fixture.request("image.generate", {{"prompt", "x"}});
        request.model = QJsonObject{{"resource", "fugue/image-model"}, {"name", "m"}};
        QCOMPARE(run(*provider, request).error.code, QStringLiteral("node_timeout"));
    }

    void aSubmitNeverRepliesInsideTheCall() {
        Fixture fixture;
        auto provider = fixture.provider({{"image_models", "gpt-image-1"}});
        (void)await<Description>([&](auto reply) { provider->describe(reply); });
        JobRequest request = fixture.request("image.generate", {{"prompt", "x"}}, {{"mask", QJsonObject{}}});
        request.model = kDraw;
        bool inside = true, replied = false;
        provider->submit(request, [&](const Result<QString> &) { replied = true; QVERIFY(!inside); });
        inside = false;
        QVERIFY(QTest::qWaitFor([&] { return replied; }, 3000));
    }

    // --- the stable-diffusion.cpp native server ---

    void aNativeServerIsFoundAndServedThroughItsJobApi() {
        Fixture fixture;
        const QJsonObject capabilities{
            {"model", QJsonObject{{"name", "FLUX.2 Klein 4B"}}},
            {"supported_modes", QJsonArray{"img_gen"}},
            {"defaults_by_mode", QJsonObject{{"img_gen", QJsonObject{{"width", 512}, {"height", 512}, {"sample_params", QJsonObject{{"sample_steps", 4}}}}}}},
            {"features_by_mode", QJsonObject{{"img_gen", QJsonObject{{"ref_images", true}, {"mask_image", false}}}}},
            {"limits", QJsonObject{{"max_width", 2048}}}};
        fixture.server.on("GET", "/sdcpp/v1/capabilities", [capabilities](const HttpRequest &request) { return needsKey(request, HttpResponse{200, json(capabilities)}); });
        fixture.server.on("POST", "/sdcpp/v1/img_gen", [](const HttpRequest &request) {
            return needsKey(request, HttpResponse{202, json({{"id", "job_1"}, {"status", "queued"}})});
        });
        const QJsonObject done{{"id", "job_1"}, {"status", "completed"},
                               {"result", QJsonObject{{"output_format", "png"},
                                                      {"images", QJsonArray{QJsonObject{{"b64_json", QString::fromLatin1(kPng.toBase64())},
                                                                                        {"generation_info", QJsonObject{{"seed", "42"}, {"width", 512}, {"height", 512}}}}}}}}};
        auto polls = std::make_shared<int>(0);
        fixture.server.on("GET", "/sdcpp/v1/jobs/job_1", [done, polls](const HttpRequest &request) {
            if (++*polls == 1)
                return needsKey(request, HttpResponse{200, json({{"id", "job_1"}, {"status", "generating"},
                                                                 {"progress", QJsonObject{{"stage", "sampling"}, {"completed", 1}, {"total", 4}, {"unit", "steps"}}}})});
            return needsKey(request, HttpResponse{200, json(done)});
        });
        auto provider = fixture.provider();
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY2(described.ok(), qPrintable(described.ok() ? QString() : described.error().message));
        QCOMPARE(described->engine, QStringLiteral("sdcpp@127.0.0.1"));
        const OfferedModel model = described->offer("image.generate")->models.first();
        QCOMPARE(model.handle, (QJsonObject{{"resource", "fugue/image-model"}, {"name", "FLUX.2 Klein 4B"}}));
        QCOMPARE(model.defaults.value("steps").toInt(), 4);
        QCOMPARE(model.limits.value("max_width").toInt(), 2048);
        QVERIFY(described->offer("image.edit")); // the server takes reference images
        QVERIFY(!described->offer("text.generate"));
        QVERIFY(!model.properties.contains("inputs"));

        JobRequest request = fixture.request("image.generate", {{"prompt", "a dot"}, {"seed", 42}, {"steps", 4}, {"width", 512}, {"height", 512}});
        request.model = model.handle;
        JobResult result;
        QCOMPARE(run(*provider, request, &result).state, QStringLiteral("done"));
        const QJsonObject body = lastBody(fixture.server, "/sdcpp/v1/img_gen");
        QCOMPARE(body.value("prompt").toString(), QStringLiteral("a dot"));
        QCOMPARE(body.value("seed").toInt(), 42);
        QCOMPARE(body.value("sample_params").toObject().value("sample_steps").toInt(), 4);
        QCOMPARE(result.used.value("seed").toInteger(), qint64(42));
        QCOMPARE(readAll(result.outputs.value("image").toObject().value("path").toString()), kPng);
        QCOMPARE(fixture.server.requestsTo("POST", "/sdcpp/v1/img_gen").first().headers.value("authorization"), "Bearer " + kKey);

        // This server lists no mask_image: a mask is refused, not dropped.
        const QJsonObject image{{"path", fixture.writeInput("a.png", kPng)}, {"format", "png"}};
        JobRequest masked = fixture.request("image.generate", {{"prompt", "x"}}, {{"image", image}, {"mask", image}});
        masked.model = model.handle;
        QCOMPARE(run(*provider, masked).error.code, QStringLiteral("mask_unsupported"));
    }

    void theApiSettingForcesADialect() {
        Fixture fixture;
        fixture.server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) { return HttpResponse{200, json({{"model", QJsonObject{{"name", "m"}}}})}; });
        fixture.service({"gpt-4o-mini"});
        auto openai = fixture.provider({{"api", "openai"}});
        QVERIFY(await<Description>([&](auto reply) { openai->describe(reply); }).value().offer("text.generate"));
        auto bad = fixture.provider({{"api", "soap"}});
        const auto described = await<Description>([&](auto reply) { bad->describe(reply); });
        QCOMPARE(described.error().code, QStringLiteral("invalid_document"));
        QVERIFY(!described.error().valid.isEmpty());
    }

    void anHttpInstanceIsMadeByTheRegistry() {
        ProviderInstance instance;
        instance.name = QStringLiteral("My endpoint");
        instance.type = QStringLiteral("http");
        instance.address = QStringLiteral("http://127.0.0.1:1/v1");
        const auto provider = createProvider(instance, QDir::tempPath() + "/no-such-modules");
        QVERIFY(provider.ok());
        QVERIFY(dynamic_cast<HttpProvider *>(provider.value().get()));
    }
};

FUGUE_TEST_MAIN(TestHttpProvider, QCoreApplication)
#include "test_http_provider.moc"
