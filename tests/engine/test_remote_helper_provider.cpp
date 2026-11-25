// The fugue-helper provider on a paired host (spec 8.8 http transport, 8.10).
//
// Seam: the Provider interface (8.11) of RemoteHelperProvider, against a fake host that
// implements the contract (`POST <address>/fugue/v1/<op>`, JSON, bearer key; results fetched
// with `GET .../result/<job>/<i>`). Nothing here talks to a real machine.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/providers.h"
#include "engine/remote_helper_provider.h"
#include "fake_http.h"
#include "provider_support.h"

using namespace fugue;
using fugue::test::await;
using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

const QByteArray kKey = "tailnet-key-0123";

QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject parsed(const QByteArray &body) { return QJsonDocument::fromJson(body).object(); }

void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

template <typename T> QString why(const Result<T> &result) { return result.ok() ? QString() : result.error().message; }

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

HttpResponse failure(int status, const QString &code, const QString &message) {
    return HttpResponse{status, json({{"error", QJsonObject{{"code", code}, {"message", message}}}})};
}

// The host: answers an operation when the bearer key is right, else 401.
class Host {
public:
    Host() { server.listen(); }

    void op(const QByteArray &name, std::function<HttpResponse(const QJsonObject &)> handler) {
        server.on("POST", "/fugue/v1/" + name, [this, handler](const HttpRequest &request) {
            if (request.headers.value("authorization") != "Bearer " + kKey)
                return failure(401, QStringLiteral("unauthorized"), QStringLiteral("Bad key."));
            return handler(parsed(request.body));
        });
    }
    void file(const QByteArray &job, int index, const QByteArray &bytes) {
        server.on("GET", "/fugue/v1/result/" + job + "/" + QByteArray::number(index),
                  [this, bytes](const HttpRequest &request) {
                      if (request.headers.value("authorization") != "Bearer " + kKey)
                          return failure(401, QStringLiteral("unauthorized"), QStringLiteral("Bad key."));
                      return HttpResponse{200, bytes, "application/octet-stream"};
                  });
    }
    QJsonObject last(const QByteArray &name) const {
        const QList<HttpRequest> sent = server.requestsTo("POST", "/fugue/v1/" + name);
        return sent.isEmpty() ? QJsonObject() : parsed(sent.last().body);
    }
    int count(const QByteArray &name) const { return int(server.requestsTo("POST", "/fugue/v1/" + name).size()); }

    FakeHttpServer server;
};

QJsonObject describeBody() {
    return QJsonObject{{"protocol", 1},
                       {"engine", "sdcpp"},
                       {"shares_filesystem", false},
                       {"backend", "cuda"},
                       {"capabilities", QJsonArray{"text.encode", "latent.sample", "latent.decode", "latent.encode"}},
                       {"free_memory_bytes", 96000000000.0},
                       {"host", "gx10"}};
}

} // namespace

class TestRemoteHelperProvider : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    ProviderInstance instance(const Host &host, const QByteArray &key = kKey) const {
        ProviderInstance made;
        made.name = QStringLiteral("gx10");
        made.type = QStringLiteral("fugue-helper");
        made.address = QString::fromLatin1(host.server.origin());
        made.key = QString::fromLatin1(key);
        return made;
    }

    std::shared_ptr<Provider> provider(const Host &host, const QByteArray &key = kKey) const {
        const Result<std::shared_ptr<Provider>> made = createProvider(instance(host, key));
        return made ? made.value() : nullptr;
    }

    JobRequest sample(const QString &outputDir) const {
        JobRequest request;
        request.capability = QStringLiteral("latent.sample");
        request.params = QJsonObject{{"context", QJsonObject{{"diffusion_model_path", QJsonObject{{"name", "m.gguf"}, {"sha256", "sha256:ab"}}}}},
                                     {"seed", 7}};
        request.outputDir = outputDir;
        return request;
    }

    QString newDir(const QString &name) const {
        const QString path = m_dir.filePath(name);
        QDir().mkpath(path);
        return path;
    }

    JobStatus waitDone(Provider &provider, const QString &job) {
        JobStatus last;
        const bool over = QTest::qWaitFor([&] {
            const Result<JobStatus> now = await<JobStatus>([&](Reply<JobStatus> reply) { provider.status(job, reply); });
            if (now)
                last = *now;
            return last.isTerminal();
        }, 5000);
        Q_UNUSED(over);
        return last;
    }

private slots:
    void describesAHostThatDoesNotShareTheFilesystem() {
        Host host;
        host.op("describe", [](const QJsonObject &) { return HttpResponse{200, json(describeBody())}; });
        const auto remote = provider(host);
        QVERIFY(remote);
        const Result<Description> described = await<Description>([&](Reply<Description> r) { remote->describe(r); });
        QVERIFY2(described.ok(), qPrintable(why(described)));
        QCOMPARE(described->engine, QStringLiteral("sdcpp@gx10")); // per host: the cache never mixes machines
        QVERIFY(!described->sharesFilesystem);
        QCOMPARE(described->freeMemoryBytes.value_or(-1), qint64(96000000000));
        for (const char *name : {"text.encode", "latent.sample", "latent.decode", "latent.encode"}) {
            const QString capability = QString::fromLatin1(name);
            const CapabilityOffer *offer = described->offer(capability);
            QVERIFY2(offer, qPrintable(capability));
            QCOMPARE(offer->models.size(), 1);
            QVERIFY(offer->models.first().handle.isEmpty());
            QCOMPARE(offer->models.first().status, ModelStatus::Ready);
        }
    }

    // #395: the probe holds the real provider until its reply is over; letting go inside the reply deleted the reply being delivered.
    void theProbeOfARealRemoteProviderEndsCleanly() {
        Host host;
        host.op("describe", [](const QJsonObject &) { return HttpResponse{200, json(describeBody())}; });
        ProviderInstance paired;
        paired.name = QStringLiteral("gx10");
        paired.type = QStringLiteral("fugue-helper");
        paired.address = QStringLiteral("http://gx10.example:8787"); // named by the probe, but made by `make` at the fake
        const auto make = [&](const ProviderInstance &) { return createProvider(instance(host)); };
        for (int round = 0; round < 3; ++round) {
            QString got = QStringLiteral("unset");
            probePairedHelper({paired}, [&got](const QString &name) { got = name; }, make);
            QTRY_COMPARE_WITH_TIMEOUT(got, QStringLiteral("gx10"), 5000);
            QTest::qWait(50); // the deferred release, and any deleteLater, run
        }
    }

    void aKeyTheHostRefusesIsUnreachableWithAMessage() {
        Host host;
        host.op("describe", [](const QJsonObject &) { return HttpResponse{200, json(describeBody())}; });
        const auto remote = provider(host, "wrong");
        const Result<Description> described = await<Description>([&](Reply<Description> r) { remote->describe(r); });
        QVERIFY(!described.ok());
        QCOMPARE(described.errors().first().code, QStringLiteral("provider_unreachable"));
        QVERIFY2(described.errors().first().message.contains(QStringLiteral("key")), qPrintable(why(described)));
    }

    void aHostThatIsNotThereIsUnreachable() {
        Host host;
        const auto remote = provider(host);
        host.server.close();
        const Result<Description> described = await<Description>([&](Reply<Description> r) { remote->describe(r); });
        QVERIFY(!described.ok());
        QCOMPARE(described.errors().first().code, QStringLiteral("provider_unreachable"));
        QVERIFY(described.errors().first().message.contains(QStringLiteral("127.0.0.1")));
        const Result<QString> submitted = await<QString>([&](Reply<QString> r) { remote->submit(sample(newDir("none")), r); });
        QCOMPARE(submitted.errors().first().code, QStringLiteral("provider_unreachable"));
    }

    void anInstanceWithoutAnAddressIsStillTheLocalHelper() {
        ProviderInstance local;
        local.name = QStringLiteral("Fugue helper");
        local.type = QStringLiteral("fugue-helper");
        const auto made = createProvider(local);
        QVERIFY(made);
        QVERIFY(!dynamic_cast<RemoteHelperProvider *>(made->get()));
        ProviderInstance remote = local;
        remote.address = QStringLiteral("http://100.84.169.13:8787");
        remote.key = QStringLiteral("k");
        const auto madeRemote = createProvider(remote);
        QVERIFY(madeRemote);
        QVERIFY(dynamic_cast<RemoteHelperProvider *>(madeRemote->get()));
    }

    void submitSendsFilesAsDataAndHandlesAsTheyAre() {
        Host host;
        host.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j1"}})}; });
        const auto remote = provider(host);
        const QString cond = m_dir.filePath("c.bin"), latent = m_dir.filePath("l.bin"), a = m_dir.filePath("a.png"),
                      b = m_dir.filePath("b.JPG");
        writeFile(cond, "COND");
        writeFile(latent, "LATENT");
        writeFile(a, "PNGDATA");
        writeFile(b, "JPGDATA");
        JobRequest request = sample(newDir("out1"));
        request.inputs = QJsonObject{{"conditioning", QJsonObject{{"path", cond}}},
                                     {"init_latent", QJsonObject{{"path", latent}}},
                                     {"images", QJsonArray{QJsonObject{{"path", a}}, QJsonObject{{"path", b}}}}};
        request.params.insert("loras", QJsonArray{QJsonObject{{"path", QJsonObject{{"name", "l.safetensors"}, {"sha256", "sha256:cd"}, {"size", 5}}}, {"multiplier", 0.8}}});
        const Result<QString> job = await<QString>([&](Reply<QString> r) { remote->submit(request, r); });
        QVERIFY2(job.ok(), qPrintable(why(job)));
        const QJsonObject sent = host.last("submit");
        QCOMPARE(sent.value("capability").toString(), QStringLiteral("latent.sample"));
        QVERIFY(!sent.contains("output_dir"));
        QCOMPARE(sent.value("params"), request.params);
        const QJsonObject inputs = sent.value("inputs").toObject();
        const auto data = [](const QJsonObject &object) { return QByteArray::fromBase64(object.value("data").toString().toLatin1()); };
        QCOMPARE(data(inputs.value("conditioning").toObject()), QByteArray("COND"));
        QCOMPARE(inputs.value("conditioning").toObject().value("format").toString(), QStringLiteral("conditioning"));
        QCOMPARE(data(inputs.value("init_latent").toObject()), QByteArray("LATENT"));
        QCOMPARE(inputs.value("init_latent").toObject().value("format").toString(), QStringLiteral("latent"));
        const QJsonArray images = inputs.value("images").toArray();
        QCOMPARE(images.size(), 2);
        QCOMPARE(data(images.at(0).toObject()), QByteArray("PNGDATA"));
        QCOMPARE(images.at(0).toObject().value("format").toString(), QStringLiteral("png"));
        QCOMPARE(images.at(1).toObject().value("format").toString(), QStringLiteral("jpg"));
        QVERIFY(!inputs.value("conditioning").toObject().contains("path"));
    }

    void referenceImagesInTheParamsGoAsDataToo() {
        Host host;
        host.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j1"}})}; });
        const auto remote = provider(host);
        const QString ref = m_dir.filePath("ref.png");
        writeFile(ref, "REF");
        JobRequest request = sample(newDir("out1b"));
        request.params.insert("ref_images", QJsonArray{QJsonObject{{"image", ref}}});
        QVERIFY(await<QString>([&](Reply<QString> r) { remote->submit(request, r); }).ok());
        const QJsonObject image = host.last("submit").value("params").toObject().value("ref_images").toArray().at(0).toObject().value("image").toObject();
        QCOMPARE(QByteArray::fromBase64(image.value("data").toString().toLatin1()), QByteArray("REF"));
        QCOMPARE(image.value("format").toString(), QStringLiteral("png"));
    }

    // #173: a sample's mask and its picture are PNG files the host reads from the job's work dir.
    void aMaskAndItsPictureGoAsPngData() {
        Host host;
        host.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j1"}})}; });
        const auto remote = provider(host);
        const QString mask = m_dir.filePath("mask.png"), picture = m_dir.filePath("masked-image.png");
        writeFile(mask, "MASK");
        writeFile(picture, "PICTURE");
        JobRequest request = sample(newDir("out1c"));
        request.inputs = QJsonObject{{"mask", QJsonObject{{"path", mask}}}, {"image", QJsonObject{{"path", picture}}}};
        QVERIFY(await<QString>([&](Reply<QString> r) { remote->submit(request, r); }).ok());
        const QJsonObject inputs = host.last("submit").value("inputs").toObject();
        QCOMPARE(QByteArray::fromBase64(inputs.value("mask").toObject().value("data").toString().toLatin1()), QByteArray("MASK"));
        QCOMPARE(inputs.value("mask").toObject().value("format").toString(), QStringLiteral("png"));
        QCOMPARE(QByteArray::fromBase64(inputs.value("image").toObject().value("data").toString().toLatin1()), QByteArray("PICTURE"));
        QCOMPARE(inputs.value("image").toObject().value("format").toString(), QStringLiteral("png"));
    }

    void aMissingInputFileFailsTheSubmit() {
        Host host;
        host.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j1"}})}; });
        const auto remote = provider(host);
        JobRequest request = sample(newDir("out2"));
        request.inputs = QJsonObject{{"conditioning", QJsonObject{{"path", m_dir.filePath("gone.bin")}}}};
        const Result<QString> job = await<QString>([&](Reply<QString> r) { remote->submit(request, r); });
        QVERIFY(!job.ok());
        QCOMPARE(job.errors().first().code, QStringLiteral("node_failed"));
        QCOMPARE(host.count("submit"), 0);
    }

    void progressIsPolledAndTheResultIsDownloadedIntoTheOutputFolder() {
        Host host;
        int polls = 0;
        host.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j1"}})}; });
        host.op("status", [&polls](const QJsonObject &) {
            ++polls;
            return HttpResponse{200, json(polls < 3 ? QJsonObject{{"state", "running"}, {"progress", 0.5}, {"stage", "sampling"}}
                                                    : QJsonObject{{"state", "done"}, {"progress", 1.0}})};
        });
        host.op("result", [](const QJsonObject &) {
            return HttpResponse{200, json({{"outputs", QJsonObject{{"image", QJsonObject{{"file", 0}, {"format", "png"}, {"size", 5}}},
                                                                   {"latents", QJsonArray{QJsonObject{{"file", 1}, {"format", "latent"}, {"size", 3}}}},
                                                                   {"note", "text"}}},
                                           {"used", QJsonObject{{"seed", 7}}}})};
        });
        host.file("j1", 0, "IMAGE");
        host.file("j1", 1, "LAT");
        const auto remote = provider(host);
        QList<JobStatus> seen;
        connect(remote.get(), &Provider::statusChanged, this, [&](const QString &, const JobStatus &status) { seen.append(status); });
        const QString out = newDir("out3");
        const Result<QString> job = await<QString>([&](Reply<QString> r) { remote->submit(sample(out), r); });
        QVERIFY(job.ok());
        const JobStatus done = waitDone(*remote, *job);
        QCOMPARE(done.state, QStringLiteral("done"));
        QVERIFY(std::any_of(seen.cbegin(), seen.cend(), [](const JobStatus &s) { return s.state == QLatin1String("running") && qFuzzyCompare(s.progress, 0.5) && s.stage == QLatin1String("sampling"); }));
        QCOMPARE(seen.last().state, QStringLiteral("done"));
        const Result<JobResult> result = await<JobResult>([&](Reply<JobResult> r) { remote->result(*job, r); });
        QVERIFY(result.ok());
        const QJsonObject image = result->outputs.value("image").toObject();
        QCOMPARE(image.value("format").toString(), QStringLiteral("png"));
        QVERIFY(image.value("path").toString().startsWith(out));
        QCOMPARE(readFile(image.value("path").toString()), QByteArray("IMAGE"));
        const QJsonObject latent = result->outputs.value("latents").toArray().at(0).toObject();
        QCOMPARE(readFile(latent.value("path").toString()), QByteArray("LAT"));
        QVERIFY(latent.value("path").toString().startsWith(out));
        QCOMPARE(result->outputs.value("note").toString(), QStringLiteral("text"));
        QCOMPARE(result->used.value("seed").toInt(), 7);
    }

    // A video latent's decode (#105) is a video file of its own, in the engine's output folder too.
    void aDecodedVideoIsDownloadedAsAY4mFile() {
        Host host;
        host.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j1"}})}; });
        host.op("status", [](const QJsonObject &) { return HttpResponse{200, json({{"state", "done"}, {"progress", 1.0}})}; });
        host.op("result", [](const QJsonObject &) {
            return HttpResponse{200, json({{"outputs", QJsonObject{{"video", QJsonObject{{"file", 0}, {"format", "y4m"}, {"size", 6}}}}}})};
        });
        host.file("j1", 0, "FRAMES");
        const auto remote = provider(host);
        const QString out = newDir("out-video");
        const Result<QString> job = await<QString>([&](Reply<QString> r) { remote->submit(sample(out), r); });
        QVERIFY(job.ok());
        QCOMPARE(waitDone(*remote, *job).state, QStringLiteral("done"));
        const Result<JobResult> result = await<JobResult>([&](Reply<JobResult> r) { remote->result(*job, r); });
        QVERIFY(result.ok());
        const QJsonObject video = result->outputs.value("video").toObject();
        QCOMPARE(video.value("format").toString(), QStringLiteral("y4m"));
        QVERIFY(video.value("path").toString().endsWith(QLatin1String(".y4m")));
        QCOMPARE(readFile(video.value("path").toString()), QByteArray("FRAMES"));
    }

    // A host's names are data: none makes a file outside the job's folder, none is taken without a size.
    void aHostsOutputNamesNeverLeaveTheOutputFolder() {
        Host host;
        host.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j1"}})}; });
        host.op("status", [](const QJsonObject &) { return HttpResponse{200, json({{"state", "done"}, {"progress", 1.0}})}; });
        host.op("result", [](const QJsonObject &) {
            return HttpResponse{200, json({{"outputs", QJsonObject{{"../../escaped", QJsonObject{{"file", 0}, {"format", "../x/png"}, {"size", 3}}},
                                                                   {"/tmp/absolute", QJsonObject{{"file", 1}, {"format", "png"}, {"size", 3}}}}},
                                           {"used", QJsonObject{}}})};
        });
        host.file("j1", 0, "ONE");
        host.file("j1", 1, "TWO");
        const auto remote = provider(host);
        const QString out = newDir("out-hostile");
        const Result<QString> job = await<QString>([&](Reply<QString> r) { remote->submit(sample(out), r); });
        QVERIFY(job.ok());
        QCOMPARE(waitDone(*remote, *job).state, QStringLiteral("done"));
        const Result<JobResult> result = await<JobResult>([&](Reply<JobResult> r) { remote->result(*job, r); });
        QVERIFY(result.ok());
        for (const QString &name : result->outputs.keys()) {
            const QString path = result->outputs.value(name).toObject().value("path").toString();
            QVERIFY2(QFileInfo(path).absolutePath() == QFileInfo(out).absoluteFilePath(), qPrintable(path));
            QVERIFY(QFileInfo(path).exists());
        }
        QVERIFY(!QFileInfo::exists(m_dir.filePath("../escaped.bin")));
    }

    void aDownloadWithoutASizeFailsTheJob() {
        Host host;
        host.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j1"}})}; });
        host.op("status", [](const QJsonObject &) { return HttpResponse{200, json({{"state", "done"}, {"progress", 1.0}})}; });
        host.op("result", [](const QJsonObject &) {
            return HttpResponse{200, json({{"outputs", QJsonObject{{"image", QJsonObject{{"file", 0}, {"format", "png"}}}}}, {"used", QJsonObject{}}})};
        });
        host.file("j1", 0, "IMAGE");
        const auto remote = provider(host);
        const Result<QString> job = await<QString>([&](Reply<QString> r) { remote->submit(sample(newDir("out-nosize")), r); });
        QVERIFY(job.ok());
        QCOMPARE(waitDone(*remote, *job).state, QStringLiteral("error"));
    }

    void aShortDownloadFailsTheJob() {
        Host host;
        host.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j1"}})}; });
        host.op("status", [](const QJsonObject &) { return HttpResponse{200, json({{"state", "done"}})}; });
        host.op("result", [](const QJsonObject &) {
            return HttpResponse{200, json({{"outputs", QJsonObject{{"image", QJsonObject{{"file", 0}, {"format", "png"}, {"size", 50}}}}}})};
        });
        host.file("j1", 0, "IMAGE");
        const auto remote = provider(host);
        const Result<QString> job = await<QString>([&](Reply<QString> r) { remote->submit(sample(newDir("out4")), r); });
        const JobStatus over = waitDone(*remote, *job);
        QCOMPARE(over.state, QStringLiteral("error"));
        QCOMPARE(over.error.code, QStringLiteral("node_failed"));
    }

    void modelMissingAndOutOfMemoryKeepTheirCodes() {
        Host host;
        host.op("submit", [](const QJsonObject &) { return failure(404, QStringLiteral("model_missing"), QStringLiteral("No m.gguf on gx10.")); });
        const auto remote = provider(host);
        Result<QString> job = await<QString>([&](Reply<QString> r) { remote->submit(sample(newDir("out5")), r); });
        QVERIFY(!job.ok());
        QCOMPARE(job.errors().first().code, QStringLiteral("model_missing"));
        QVERIFY(job.errors().first().message.contains(QStringLiteral("m.gguf")));

        Host second;
        second.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j2"}})}; });
        second.op("status", [](const QJsonObject &) {
            return HttpResponse{200, json({{"state", "error"}, {"error", QJsonObject{{"code", "out_of_memory"}, {"message", "No room."}}}})};
        });
        const auto other = provider(second);
        job = await<QString>([&](Reply<QString> r) { other->submit(sample(newDir("out6")), r); });
        QVERIFY(job.ok());
        const JobStatus over = waitDone(*other, *job);
        QCOMPARE(over.state, QStringLiteral("error"));
        QCOMPARE(over.error.code, QStringLiteral("out_of_memory"));

        Host third;
        third.op("submit", [](const QJsonObject &) { return failure(500, QStringLiteral("weird"), QStringLiteral("Boom.")); });
        const auto last = provider(third);
        job = await<QString>([&](Reply<QString> r) { last->submit(sample(newDir("out7")), r); });
        QCOMPARE(job.errors().first().code, QStringLiteral("node_failed"));
    }

    void cancelIsSentAndTheCancelledStatusFollows() {
        Host host;
        bool cancelled = false;
        host.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j1"}})}; });
        host.op("cancel", [&cancelled](const QJsonObject &) {
            cancelled = true;
            return HttpResponse{200, json({{"state", "running"}})};
        });
        host.op("status", [&cancelled](const QJsonObject &) {
            return HttpResponse{200, json({{"state", cancelled ? "cancelled" : "running"}})};
        });
        const auto remote = provider(host);
        const Result<QString> job = await<QString>([&](Reply<QString> r) { remote->submit(sample(newDir("out8")), r); });
        QVERIFY(job.ok());
        const Result<QString> asked = await<QString>([&](Reply<QString> r) { remote->cancel(*job, r); });
        QVERIFY(asked.ok());
        QCOMPARE(host.last("cancel").value("job").toString(), QStringLiteral("j1"));
        QCOMPARE(waitDone(*remote, *job).state, QStringLiteral("cancelled"));
    }

    void aHostThatGoesAwayMidJobFailsTheJob() {
        Host host;
        host.op("submit", [](const QJsonObject &) { return HttpResponse{200, json({{"job", "j1"}})}; });
        host.op("status", [](const QJsonObject &) { return HttpResponse{200, json({{"state", "running"}})}; });
        const auto remote = provider(host);
        const Result<QString> job = await<QString>([&](Reply<QString> r) { remote->submit(sample(newDir("out9")), r); });
        QVERIFY(job.ok());
        host.server.close();
        const JobStatus over = waitDone(*remote, *job);
        QCOMPARE(over.state, QStringLiteral("error"));
        QCOMPARE(over.error.code, QStringLiteral("provider_unreachable"));
    }

    void unknownJobs() {
        Host host;
        const auto remote = provider(host);
        QCOMPARE(await<JobStatus>([&](Reply<JobStatus> r) { remote->status("nope", r); }).errors().first().code, QStringLiteral("unknown_run"));
        QCOMPARE(await<JobResult>([&](Reply<JobResult> r) { remote->result("nope", r); }).errors().first().code, QStringLiteral("unknown_run"));
    }
};

QTEST_GUILESS_MAIN(TestRemoteHelperProvider)
#include "test_remote_helper_provider.moc"
