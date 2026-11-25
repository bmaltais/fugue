// The `cloudflare` provider (spec 8.7, #407) against a fake Workers AI API on the loopback.
// Every token here is made up. Nothing reaches Cloudflare.

#include <QDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/cloudflare_provider.h"
#include "engine/providers.h"
#include "engine/server_api.h"
#include "engine/vips.h"
#include "fake_http.h"
#include "../test_shard.h"
#include "provider_support.h"

using namespace fugue;
using fugue::test::await;
using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

const QByteArray kKey = "cf-test-token-0123456789abcdef-secretsecret";
const QString kAccount = QStringLiteral("0123456789abcdef0123456789abcdef");

// A 1x1 PNG.
const QByteArray kPng = QByteArray::fromBase64(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");

const char *const kSchnell = "@cf/black-forest-labs/flux-1-schnell";
const char *const kKlein = "@cf/black-forest-labs/flux-2-klein-4b";

QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject parse(const QByteArray &bytes) { return QJsonDocument::fromJson(bytes).object(); }

QByteArray envelope() { return json({{"result", QJsonObject{{"image", QString::fromLatin1(kPng.toBase64())}}}, {"success", true}, {"errors", QJsonArray{}}, {"messages", QJsonArray{}}}); }

QByteArray fail(int code, const QString &message) {
    return json({{"success", false}, {"result", QJsonValue::Null}, {"errors", QJsonArray{QJsonObject{{"code", code}, {"message", message}}}}, {"messages", QJsonArray{}}});
}

QString runPath(const QString &model) { return "/client/v4/accounts/" + kAccount + "/ai/run/" + model; }

class Fixture {
public:
    Fixture() {
        api.listen();
        other.listen();
        for (const char *model : {kSchnell, kKlein, "@cf/acme/my-model"})
            api.on("POST", runPath(model).toLatin1(), [this](const HttpRequest &request) {
                if (request.headers.value("authorization") != "Bearer " + kKey)
                    return HttpResponse{401, fail(10000, "Authentication error: " + QString::fromLatin1(kKey))};
                return answer;
            });
        answer = HttpResponse{200, envelope()};
    }

    std::unique_ptr<CloudflareProvider> provider(const QJsonObject &settings = {}, const QByteArray &key = kKey, bool account = true) {
        ProviderInstance instance;
        instance.name = QStringLiteral("Cloudflare Workers AI");
        instance.type = QStringLiteral("cloudflare");
        instance.address = QString::fromLatin1(api.origin());
        instance.key = QString::fromLatin1(key);
        instance.settings = settings;
        if (account)
            instance.settings.insert("account_id", kAccount);
        return std::make_unique<CloudflareProvider>(instance);
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

    QJsonObject picture(const QString &name) {
        QFile file(dir.filePath(name));
        if (file.open(QIODevice::WriteOnly))
            file.write(kPng);
        return QJsonObject{{"path", dir.filePath(name)}, {"format", "png"}};
    }

    QTemporaryDir dir;
    FakeHttpServer api;
    FakeHttpServer other; // anything that is not the API: it must never be called
    HttpResponse answer;
};

JobStatus run(CloudflareProvider &provider, const JobRequest &request, JobResult *result = nullptr) {
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

class TestCloudflareProvider : public QObject {
    Q_OBJECT

private slots:
    void theShippedTableOffersItsModelsByWhatTheyDo() {
        Fixture fixture;
        auto provider = fixture.provider({{"image_models", "@cf/acme/my-model, @cf/black-forest-labs/flux-1-schnell, not-a-model, @cf/x/../y, @cf/x/%2e%2e/y"}});
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QVERIFY2(described.ok(), qPrintable(described.ok() ? QString() : described.error().message));
        QCOMPARE(described->engine, QStringLiteral("cloudflare@127.0.0.1"));
        const auto names = [](const CapabilityOffer *offer) {
            QStringList list;
            for (const OfferedModel &model : offer->models)
                list.append(model.handle.value("name").toString());
            return list;
        };
        const QStringList generate = names(described->offer("image.generate"));
        QCOMPARE(generate, (QStringList{kSchnell, kKlein, "@cf/black-forest-labs/flux-2-klein-9b", "@cf/black-forest-labs/flux-2-dev", "@cf/acme/my-model"}));
        QCOMPARE(names(described->offer("image.edit")),
                 (QStringList{kKlein, "@cf/black-forest-labs/flux-2-klein-9b", "@cf/black-forest-labs/flux-2-dev"})); // the ones with reference pictures
        const OfferedModel &first = described->offer("image.generate")->models.first();
        QCOMPARE(first.handle, (QJsonObject{{"resource", "fugue/image-model"}, {"name", kSchnell}}));
        QCOMPARE(first.status, ModelStatus::Ready);
        QVERIFY(first.title.contains("$0.0006"));
        QCOMPARE(first.defaults.value("steps").toInt(), 4);

        // Without a token or an account id nothing can run: the models say what is missing, and a job is refused before anything is sent.
        const auto keyless = fixture.provider({}, "");
        const auto noToken = await<Description>([&](auto reply) { keyless->describe(reply); });
        QCOMPARE(noToken->offer("image.generate")->models.first().status, ModelStatus::Unavailable);
        QVERIFY(noToken->offer("image.generate")->models.first().hint.contains("Settings > Providers"));
        const auto accountless = fixture.provider({}, kKey, false);
        const auto noAccount = await<Description>([&](auto reply) { accountless->describe(reply); });
        QCOMPARE(noAccount->offer("image.generate")->models.first().status, ModelStatus::Unavailable);
        QVERIFY(noAccount->offer("image.generate")->models.first().reason.contains("account id"));
        const JobStatus refused = run(*accountless, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}));
        QCOMPARE(refused.error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(refused.error.message.contains("account id"));
        QVERIFY(refused.error.hint.contains("Settings > Providers"));
        QCOMPARE(run(*keyless, fixture.request("image.generate", kSchnell, {{"prompt", "x"}})).error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(fixture.api.requests().isEmpty());
    }

    void aGenerateIsOneJsonCallWithTheTokenAndNoOtherRequest() {
        Fixture fixture;
        auto provider = fixture.provider();
        JobResult result;
        const JobStatus status = run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "a dot"}, {"width", 768}, {"seed", 7}, {"steps", 20}, {"guidance", 4.0}}), &result);
        QCOMPARE(status.state, QStringLiteral("done"));
        const QList<HttpRequest> posts = fixture.api.requestsTo("POST", runPath(kSchnell).toLatin1());
        QCOMPARE(posts.size(), 1);
        QCOMPARE(fixture.api.requests().size(), 1);
        QCOMPARE(posts.first().headers.value("authorization"), "Bearer " + kKey);
        QVERIFY(posts.first().headers.value("content-type").startsWith("application/json"));
        // schnell takes a prompt, steps (at most 8) and a seed.
        QCOMPARE(parse(posts.first().body), (QJsonObject{{"prompt", "a dot"}, {"steps", 8}, {"seed", 7}}));
        QCOMPARE(readAll(result.outputs.value("image").toObject().value("path").toString()), kPng);
        QCOMPARE(result.used.value("engine").toString(), QStringLiteral("cloudflare@") + kSchnell);
        QCOMPARE(result.used.value("seed").toInt(), 7);
        QCOMPARE(result.used.value("width").toInt(), 1); // the picture's own size
        QVERIFY(!json(result.used).contains(kKey));
        QVERIFY(fixture.other.requests().isEmpty());
    }

    void aFlux2JobIsMultipartAndAnEditCarriesItsPictures() {
        Fixture fixture;
        auto provider = fixture.provider();
        QCOMPARE(run(*provider, fixture.request("image.generate", kKlein, {{"prompt", "a dot"}, {"width", 4000}, {"height", 512}, {"seed", 5}, {"steps", 9}})).state, QStringLiteral("done"));
        const HttpRequest generated = fixture.api.requestsTo("POST", runPath(kKlein).toLatin1()).last();
        QVERIFY(generated.headers.value("content-type").startsWith("multipart/form-data"));
        const auto field = [](const HttpRequest &request, const QByteArray &name, const QByteArray &value) {
            return request.body.contains("name=\"" + name + "\"\r\n\r\n" + value + "\r\n");
        };
        QVERIFY(field(generated, "prompt", "a dot"));
        QVERIFY(field(generated, "width", "1920")); // clamped to the model's range
        QVERIFY(field(generated, "height", "512"));
        QVERIFY(field(generated, "seed", "5"));
        QVERIFY(!generated.body.contains("name=\"steps\"")); // klein runs 4 steps, always

        const QJsonObject image = fixture.picture("in.png");
        QCOMPARE(run(*provider, fixture.request("image.edit", kKlein, {{"prompt", "make it blue"}}, {{"image", image}, {"images", QJsonArray{image}}})).state, QStringLiteral("done"));
        const HttpRequest edited = fixture.api.requestsTo("POST", runPath(kKlein).toLatin1()).last();
        QVERIFY(edited.body.contains("name=\"input_image_0\"; filename=\"input_image_0.png\""));
        QVERIFY(edited.body.contains("name=\"input_image_1\""));
        QVERIFY(edited.body.toLower().contains("content-type: image/png"));
        QVERIFY(edited.body.contains("\x89PNG"));
        QVERIFY(!edited.body.contains(kKey));

        // A model with no reference pictures, a mask, and an edit with no picture are refused before anything is sent.
        const int sent = fixture.api.requests().size();
        QCOMPARE(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}, {{"image", image}})).error.code, QStringLiteral("input_invalid"));
        QCOMPARE(run(*provider, fixture.request("image.generate", kKlein, {{"prompt", "x"}}, {{"mask", image}})).error.code, QStringLiteral("mask_unsupported"));
        QCOMPARE(run(*provider, fixture.request("image.edit", kKlein, {{"prompt", "x"}})).error.code, QStringLiteral("missing_input"));
        QCOMPARE(fixture.api.requests().size(), sent);
    }

    void aBigReferencePictureIsScaledToFitUnder512KeepingItsAspect() {
        Fixture fixture;
        QVERIFY(initVips());
        const vips::VImage wide = vips::VImage::black(2048, 1024);
        wide.write_to_file(fixture.dir.filePath("wide.png").toUtf8().constData());
        auto provider = fixture.provider();
        const QJsonObject image{{"path", fixture.dir.filePath("wide.png")}, {"format", "png"}};
        QCOMPARE(run(*provider, fixture.request("image.edit", kKlein, {{"prompt", "x"}}, {{"image", image}})).state, QStringLiteral("done"));
        const QByteArray body = fixture.api.requestsTo("POST", runPath(kKlein).toLatin1()).last().body;
        const qsizetype start = body.indexOf("\x89PNG");
        QVERIFY(start > 0);
        const QSize size = serverapi::pngSize(body.mid(start, body.indexOf("\r\n--", start) - start));
        QCOMPARE(size, QSize(511, 256));
        // A picture libvips cannot read is refused before anything is sent, and more pictures than the model takes are too.
        QFile bad(fixture.dir.filePath("bad.png"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not a picture");
        bad.close();
        const int sent = fixture.api.requests().size();
        const QJsonObject broken{{"path", fixture.dir.filePath("bad.png")}, {"format", "png"}};
        QCOMPARE(run(*provider, fixture.request("image.edit", kKlein, {{"prompt", "x"}}, {{"image", broken}})).error.code, QStringLiteral("media_unreadable"));
        const QJsonArray five{image, image, image, image, image};
        const JobStatus many = run(*provider, fixture.request("image.edit", kKlein, {{"prompt", "x"}}, {{"images", five}}));
        QCOMPARE(many.error.code, QStringLiteral("input_invalid"));
        QVERIFY(many.error.message.contains("at most 4"));
        QCOMPARE(fixture.api.requests().size(), sent);
    }

    void aSeedBeyondTheSignedIntRangeIsReducedAndTheOneSentIsRecorded() {
        Fixture fixture;
        auto provider = fixture.provider();
        JobResult result;
        const qint64 seed = (1LL << 40) + 5;
        QCOMPARE(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}, {"seed", double(seed)}}), &result).state, QStringLiteral("done"));
        const qint64 sent = parse(fixture.api.requestsTo("POST", runPath(kSchnell).toLatin1()).last().body).value("seed").toDouble();
        QCOMPARE(sent, seed % 2147483647);
        QVERIFY(sent < (1LL << 31));
        QCOMPARE(qint64(result.used.value("seed").toDouble()), sent);
    }

    void aModelIdOutsideTheRuleIsNotOfferedNorSent() {
        Fixture fixture;
        auto provider = fixture.provider({{"image_models", "@cf/x/.hidden, @cf/x/-dash, @cf/x/ok"}});
        const auto described = await<Description>([&](auto reply) { provider->describe(reply); });
        QStringList names;
        for (const OfferedModel &model : described->offer("image.generate")->models)
            names.append(model.handle.value("name").toString());
        QVERIFY(names.contains("@cf/x/ok") && !names.contains("@cf/x/.hidden") && !names.contains("@cf/x/-dash"));
        const JobStatus status = run(*provider, fixture.request("image.generate", "@cf/x/../y", {{"prompt", "x"}}));
        QVERIFY(status.state == QLatin1String("error") && !status.error.code.isEmpty());
        QVERIFY(fixture.api.requests().isEmpty());
    }

    void plainHttpToARemoteHostIsRefusedAtSubmitWithNothingSent() {
        Fixture fixture;
        ProviderInstance instance;
        instance.name = QStringLiteral("Cloudflare Workers AI");
        instance.type = QStringLiteral("cloudflare");
        instance.key = QString::fromLatin1(kKey);
        instance.address = QStringLiteral("http://api.example.com");
        instance.settings.insert("account_id", kAccount);
        CloudflareProvider provider(instance);
        const JobStatus status = run(provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}));
        QCOMPARE(status.error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(status.error.hint.contains("https://"));
        QVERIFY(!json(status.error.toJson()).contains(kKey));
    }

    void anAnswerBeyondTheCapIsCutOff() {
        Fixture fixture;
        fixture.answer = HttpResponse{200, QByteArray(33 << 20, 'x')};
        auto provider = fixture.provider();
        const JobStatus status = run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}));
        QCOMPARE(status.error.code, QStringLiteral("node_failed"));
        QVERIFY(status.error.message.contains("more data"));
    }

    void anAnswerThatTricklesInIsGivenUpOnAtTheJobsDeadline() {
        // A server that sends a huge answer one byte at a time: no byte-to-byte timeout ever trips.
        QTcpServer drip;
        QVERIFY(drip.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&drip, &QTcpServer::newConnection, &drip, [&drip] {
            QTcpSocket *socket = drip.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 100000000\r\n\r\n");
                auto *timer = new QTimer(socket);
                QObject::connect(timer, &QTimer::timeout, socket, [socket] { socket->write("x"); });
                timer->start(20);
            });
        });
        Fixture fixture;
        // the job's deadline is 6 times timeout_s
        ProviderInstance instance;
        instance.name = QStringLiteral("Cloudflare Workers AI");
        instance.type = QStringLiteral("cloudflare");
        instance.key = QString::fromLatin1(kKey);
        instance.address = QStringLiteral("http://127.0.0.1:%1").arg(drip.serverPort());
        instance.settings = QJsonObject{{"account_id", kAccount}, {"timeout_s", 0.15}};
        CloudflareProvider slow(instance);
        const JobStatus status = run(slow, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}));
        QCOMPARE(status.error.code, QStringLiteral("node_timeout"));
    }

    void theTokenGoesToTheApiOriginOnlyAndARedirectIsNotFollowed() {
        Fixture fixture;
        fixture.api.on("POST", runPath(kSchnell).toLatin1(), [&](const HttpRequest &) {
            return HttpResponse{302, "", "text/plain", false, -1, {"Location: " + fixture.other.origin() + "/elsewhere"}};
        });
        auto provider = fixture.provider();
        QCOMPARE(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}})).error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(fixture.other.requests().isEmpty());
    }

    void theTokenIsNotSentOverPlainHttpToARemoteHost() {
        ProviderInstance instance;
        instance.name = QStringLiteral("Cloudflare Workers AI");
        instance.type = QStringLiteral("cloudflare");
        instance.key = QString::fromLatin1(kKey);
        instance.address = QStringLiteral("http://api.example.com");
        instance.settings.insert("account_id", kAccount);
        CloudflareProvider provider(instance);
        const auto described = await<Description>([&](auto reply) { provider.describe(reply); });
        QCOMPARE(described.error().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.error().hint.contains("https://"));
    }

    void anAccountIdThatIsNotAnIdNeverReachesTheUrl() {
        Fixture fixture;
        auto provider = fixture.provider({{"account_id", "abc/../../x"}}, kKey, false);
        QCOMPARE(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}})).error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(fixture.api.requests().isEmpty());
    }

    void cancelAbortsTheCall() {
        Fixture fixture;
        fixture.answer = HttpResponse{200, {}, "application/json", true};
        auto provider = fixture.provider();
        const auto job = await<QString>([&](auto reply) { provider->submit(fixture.request("image.generate", kSchnell, {{"prompt", "x"}}), reply); });
        QVERIFY(job.ok());
        QTRY_VERIFY(!fixture.api.requests().isEmpty());
        QCOMPARE(await<QString>([&](auto reply) { provider->cancel(job.value(), reply); }).value(), QStringLiteral("cancelled"));
        QTest::qWait(30);
        QCOMPARE(await<JobStatus>([&](auto reply) { provider->status(job.value(), reply); }).value().state, QStringLiteral("cancelled"));
    }

    void errorsMapToTheCodesOfTheSpecAndNeverCarryTheToken() {
        struct Row {
            const char *name;
            HttpResponse answer;
            QString code;
            QString says;
        };
        const QString echo = QString::fromLatin1(kKey);
        const QList<Row> rows{
            {"401", HttpResponse{401, fail(10000, "Authentication error " + echo)}, "provider_unreachable", "refused the token"},
            {"403", HttpResponse{403, fail(9109, "Account is locked")}, "provider_unreachable", "Account is locked"},
            {"model", HttpResponse{404, fail(5007, "No such model @cf/x/y")}, "model_missing", "no such model"},
            {"account", HttpResponse{404, fail(7000, "No route for that URI")}, "provider_unreachable", "account"},
            {"413", HttpResponse{413, fail(1, "too big")}, "input_invalid", "too large"},
            {"429", HttpResponse{429, fail(1, "slow down"), "application/json", false, -1, {"Retry-After: 17"}}, "node_failed", "wait 17 seconds"},
            {"neurons", HttpResponse{429, fail(4006, "you have used up your daily free allocation of 10,000 neurons")}, "node_failed", "neurons"},
            {"policy", HttpResponse{400, fail(3030, "Input prompt flagged as NSFW")}, "node_failed", "content policy"},
            {"500", HttpResponse{500, fail(5000, "boom " + echo)}, "node_failed", "boom"},
            {"oom", HttpResponse{500, fail(5000, "CUDA out of memory")}, "out_of_memory", "out of memory"},
            {"502", HttpResponse{502, "{}"}, "provider_unreachable", "not available"},
            {"400", HttpResponse{400, fail(5006, "bad prompt")}, "node_failed", "bad prompt"},
            {"success false", HttpResponse{200, fail(1, "odd")}, "node_failed", "odd"},
            {"no image", HttpResponse{200, json({{"result", QJsonObject{}}, {"success", true}})}, "node_failed", "without an image"},
            {"not a picture", HttpResponse{200, json({{"result", QJsonObject{{"image", "AAAA"}}}, {"success", true}})}, "node_failed", "not a picture"},
        };
        for (const Row &row : rows) {
            Fixture fixture;
            fixture.api.on("POST", runPath(kSchnell).toLatin1(), [&row](const HttpRequest &) { return row.answer; });
            auto provider = fixture.provider();
            const JobStatus status = run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}));
            QVERIFY2(status.state == QLatin1String("error"), row.name);
            QVERIFY2(status.error.code == row.code, qPrintable(QStringLiteral("%1: %2 (%3)").arg(row.name, status.error.code, status.error.message)));
            QVERIFY2(status.error.message.contains(row.says, Qt::CaseInsensitive), qPrintable(QStringLiteral("%1: %2").arg(row.name, status.error.message)));
            const QByteArray said = json(status.error.toJson());
            QVERIFY2(!said.contains(kKey) && !said.contains("secretsecret"), row.name);
            if (QLatin1String(row.name) == QLatin1String("401"))
                QVERIFY(status.error.hint.contains("account id"));
        }
    }

    void aRawPictureAnswerIsReadToo() {
        Fixture fixture;
        fixture.api.on("POST", runPath(kSchnell).toLatin1(), [](const HttpRequest &) { return HttpResponse{200, kPng, "image/png"}; });
        auto provider = fixture.provider();
        JobResult result;
        QCOMPARE(run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}), &result).state, QStringLiteral("done"));
        QCOMPARE(readAll(result.outputs.value("image").toObject().value("path").toString()), kPng);
    }

    void aServiceThatSaysNothingIsATimeoutNamingTheSetting() {
        Fixture fixture;
        fixture.answer = HttpResponse{200, {}, "application/json", true};
        auto provider = fixture.provider({{"timeout_s", 0.2}});
        const JobStatus status = run(*provider, fixture.request("image.generate", kSchnell, {{"prompt", "x"}}));
        QCOMPARE(status.error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(status.error.hint.contains("timeout_s"));
    }

    void aCloudflareInstanceIsMadeByTheRegistry() {
        ProviderInstance instance;
        instance.name = QStringLiteral("Cloudflare Workers AI");
        instance.type = QStringLiteral("cloudflare");
        instance.key = "k";
        const auto provider = createProvider(instance, QDir::tempPath() + "/no-such-modules");
        QVERIFY(provider.ok());
        QVERIFY(dynamic_cast<CloudflareProvider *>(provider.value().get()));
        QCOMPARE(cloudflareShippedModels().size(), 4);
    }
};

FUGUE_TEST_MAIN(TestCloudflareProvider, QCoreApplication)
#include "test_cloudflare_provider.moc"
