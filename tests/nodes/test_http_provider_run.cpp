// The `http` provider end to end (spec 8.7, #401): the shipped `fugue` runs Generate Image into Describe Image
// through one http instance of providers.json, against a fake OpenAI-compatible service on the loopback. The key
// is made up; it must reach the service as a bearer token and appear in no file the run wrote.

#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include "../test_paths.h"
#include "../engine/fake_http.h"

using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

const QByteArray kKey = "sk-made-up-key-4f9a2c71e0b3";

// A 1x1 PNG.
const QByteArray kPng = QByteArray::fromBase64(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");

QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

bool writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

HttpResponse withKey(const HttpRequest &request, const HttpResponse &fine) {
    if (request.headers.value("authorization") != "Bearer " + kKey)
        return HttpResponse{401, compact({{"error", QJsonObject{{"message", "bad key"}}}})};
    return fine;
}

} // namespace

class TestHttpProviderRun : public QObject {
    Q_OBJECT

private slots:
    void oneInstanceGeneratesAndDescribesAndTheKeyStaysInProvidersJson() {
        QTemporaryDir dir;
        FakeHttpServer server;
        QVERIFY(server.listen());
        server.on("GET", "/v1/models", [](const HttpRequest &request) {
            return withKey(request, HttpResponse{200, compact({{"data", QJsonArray{QJsonObject{{"id", "gpt-image-1"}}, QJsonObject{{"id", "gpt-4o-mini"}}}}})});
        });
        server.on("POST", "/v1/images/generations", [](const HttpRequest &request) {
            return withKey(request, HttpResponse{200, compact({{"data", QJsonArray{QJsonObject{{"b64_json", QString::fromLatin1(kPng.toBase64())}}}}})});
        });
        server.on("POST", "/v1/chat/completions", [](const HttpRequest &request) {
            const QByteArray chunk = compact({{"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{{"content", "A tiny dot."}}}, {"finish_reason", "stop"}}}}});
            return withKey(request, HttpResponse{200, "data: " + chunk + "\n\ndata: [DONE]\n\n", "text/event-stream"});
        });

        const QString config = dir.filePath("config/fugue/providers.json");
        QVERIFY(writeFile(config, compact({{"providers", QJsonArray{QJsonObject{{"name", "My endpoint"}, {"type", "http"},
                                                                                 {"address", QString::fromLatin1(server.origin()) + "/v1"},
                                                                                 {"key", QString::fromLatin1(kKey)}}}}})));
        QFile::setPermissions(config, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const QJsonObject generate{{"type", "fugue/generate-image"}, {"version", "1.0.0"},
                                   {"inputs", QJsonObject{{"prompt", "a dot"}, {"width", 1024}, {"height", 1024}}}};
        const QJsonObject describe{{"type", "fugue/describe-image"}, {"version", "1.0.0"},
                                   {"inputs", QJsonObject{{"image", QJsonObject{{"from", "g.image"}}}}}};
        QVERIFY(writeFile(dir.filePath("flow.fugue"),
                          compact({{"fugue", 1}, {"graph", QJsonObject{{"nodes", QJsonObject{{"g", generate}, {"d", describe}}}}}})));

        const auto fugue = [&](const QStringList &args, QByteArray *out) {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            for (const char *name : {"QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "FUGUE_NODE_MANIFESTS", "FUGUE_HOME"})
                env.remove(QString::fromLatin1(name));
            env.insert("HOME", dir.filePath("home"));
            env.insert("XDG_CACHE_HOME", dir.filePath("cache"));
            env.insert("XDG_STATE_HOME", dir.filePath("state"));
            env.insert("XDG_CONFIG_HOME", dir.filePath("config"));
            env.insert("XDG_DATA_HOME", dir.filePath("data"));
            QProcess process;
            process.setProcessEnvironment(env);
            process.setWorkingDirectory(dir.path());
            process.start(fugueBinary(), args);
            if (!process.waitForStarted())
                return -1;
            QElapsedTimer clock;
            clock.start();
            // The fake server lives in this process, so the loop must run while the binary works.
            while (process.state() != QProcess::NotRunning && clock.elapsed() < 60000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (process.state() != QProcess::NotRunning) {
                process.kill();
                process.waitForFinished();
                return -2;
            }
            *out = process.readAllStandardOutput() + process.readAllStandardError();
            return process.exitCode();
        };

        QByteArray out;
        QCOMPARE(fugue({"run", dir.filePath("flow.fugue"), "--to", "d", "--wait", "--json"}, &out), 0);
        QVERIFY2(!out.contains(kKey), "the key is in the run's output");
        QJsonObject last;
        for (const QByteArray &line : out.trimmed().split('\n'))
            if (!line.isEmpty())
                last = QJsonDocument::fromJson(line).object();
        QCOMPARE(last.value("event").toString(), QStringLiteral("run.done"));
        QCOMPARE(last.value("status").toString(), QStringLiteral("ok"));
        const QString run = QJsonDocument::fromJson(out.trimmed().split('\n').first()).object().value("run").toString();

        QByteArray result;
        QCOMPARE(fugue({"runs", "result", run, "--json"}, &result), 0);
        const QJsonObject outputs = QJsonDocument::fromJson(result).object().value("data").toObject().value("outputs").toObject();
        QVERIFY2(outputs.value("d").toObject().value("text").toString() == "A tiny dot.", result.constData());
        QCOMPARE(QJsonDocument::fromJson(server.requestsTo("POST", "/v1/images/generations").first().body).object().value("model").toString(),
                 QStringLiteral("gpt-image-1"));
        QCOMPARE(outputs.value("d").toObject().value("model").toObject().value("name").toString(), QStringLiteral("gpt-4o-mini"));

        // Every call carried the key as a bearer token.
        for (const HttpRequest &request : server.requests())
            QCOMPARE(request.headers.value("authorization"), "Bearer " + kKey);
        QVERIFY(!server.requestsTo("POST", "/v1/images/generations").isEmpty());
        QVERIFY(!server.requestsTo("POST", "/v1/chat/completions").isEmpty());

        // No file the run left (run.json, events, the cache, previews) holds the key, nor does the output.
        QVERIFY(!result.contains(kKey));
        int scanned = 0;
        QDirIterator files(dir.path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            if (path == config)
                continue;
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY2(!file.readAll().contains(kKey), qPrintable("the key is in " + path));
            ++scanned;
        }
        QVERIFY(scanned > 3); // run.json and its siblings were really looked at
    }
};

QTEST_MAIN(TestHttpProviderRun)
#include "test_http_provider_run.moc"
