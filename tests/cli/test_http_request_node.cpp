// The HTTP Request node of the pack examples/packs/io.github.bmaltais.fugue-http (#406), against a
// fake server on the loopback. Nothing here reaches the internet.
//
// Seam: the node's own program (request.py through run.sh), driven with the command protocol, for the
// request and answer rules; and the real `fugue` for what only the engine decides: the trust prompt
// shows `network`, a validation never calls the server, and a run that reaches the node does.
// The fake server lives in this process, so every subprocess is waited for with the event loop.

#include <QDir>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "../engine/fake_http.h"

using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

const QString kPack = QStringLiteral(FUGUE_SOURCE_DIR "/examples/packs/io.github.bmaltais.fugue-http");
const QString kType = QStringLiteral("io.github.bmaltais.fugue-http/request");

struct Answer {
    bool ok = false;
    QString code;
    QString message;
    QJsonObject outputs;
};

QByteArray json(const QJsonValue &value) {
    return value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                           : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
}

} // namespace

class TestHttpRequestNode : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    FakeHttpServer m_server;

    // Runs `program` with `args` and `input` on stdin; waits with the event loop so the server answers.
    QByteArray spawn(const QString &program, const QStringList &args, const QByteArray &input, QProcessEnvironment env,
                     int *exitCode = nullptr, int waitMs = 60000) {
        QProcess process;
        process.setProcessEnvironment(env);
        process.start(program, args);
        process.write(input);
        process.closeWriteChannel();
        QEventLoop loop; // the fake server answers inside this loop
        QTimer::singleShot(waitMs, &loop, &QEventLoop::quit);
        QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
        if (process.state() != QProcess::NotRunning)
            loop.exec();
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
            qWarning("timed out: %s", qPrintable(program));
        }
        if (exitCode)
            *exitCode = process.exitCode();
        return process.readAllStandardOutput();
    }

    // One run of the node program: the inputs the engine would give it, with the defaults filled in.
    Answer call(const QString &path, QJsonObject inputs = {}) {
        if (!inputs.contains("url"))
            inputs.insert("url", QString(m_server.origin()) + path);
        const QJsonObject request{{"protocol", 1}, {"op", "run"}, {"id", "r1:request"}, {"inputs", inputs}};
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("FUGUE_PROTOCOL", "1");
        const QByteArray out = spawn(kPack + "/request/run.sh", {}, json(request), env);
        const QJsonObject line = QJsonDocument::fromJson(out.trimmed()).object();
        Answer answer;
        answer.ok = line.value("op").toString() == "result";
        answer.code = line.value("code").toString();
        answer.message = line.value("message").toString();
        answer.outputs = line.value("outputs").toObject();
        return answer;
    }

    static QJsonObject postJson(const QString &body) {
        return {{"method", "POST"}, {"body_kind", "json"}, {"body", body}};
    }

    // The fugue binary, in a scratch home, never the owner's.
    QProcessEnvironment fugueEnv() const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.remove("DISPLAY");
        env.remove("WAYLAND_DISPLAY");
        env.insert("FUGUE_HOME", m_dir.filePath("fugue-home"));
        env.insert("XDG_DATA_HOME", m_dir.filePath("data"));
        env.insert("XDG_STATE_HOME", m_dir.filePath("state"));
        env.insert("XDG_CACHE_HOME", m_dir.filePath("cache"));
        env.insert("XDG_CONFIG_HOME", m_dir.filePath("config"));
        env.insert("HOME", m_dir.filePath("home"));
        return env;
    }

    QByteArray fugue(const QStringList &args, int *exitCode = nullptr) {
        return spawn(FUGUE_BINARY, args, {}, fugueEnv(), exitCode, 120000);
    }

    static QString shellQuote(const QString &text) { return "'" + QString(text).replace("'", "'\\''") + "'"; }

private slots:
    void initTestCase() {
        QVERIFY(m_server.listen());
        m_server.on("GET", "/lookup", [](const HttpRequest &) {
            return HttpResponse{200, json(QJsonObject{{"data", QJsonObject{{"items", QJsonArray{QJsonObject{{"name", "first"}},
                                                                                              QJsonObject{{"name", "second"}}}},
                                                                      {"count", 3},
                                                                      {"ok", true},
                                                                      {"owner", QJsonObject{{"id", 7}}}}}})};
        });
        m_server.on("POST", "/hook", [](const HttpRequest &request) {
            return HttpResponse{201, json(QJsonObject{{"type", QString::fromUtf8(request.headers.value("content-type"))},
                                                      {"key", QString::fromUtf8(request.headers.value("x-api-key"))},
                                                      {"got", QString::fromUtf8(request.body)}})};
        });
        m_server.on("POST", "/upload", [](const HttpRequest &) { return HttpResponse{200, json(QJsonObject{{"id", "u1"}})}; });
        m_server.on("GET", "/missing", [](const HttpRequest &) { return HttpResponse{404, "{\"error\":\"nope\"}"}; });
        m_server.on("GET", "/hold", [](const HttpRequest &) {
            HttpResponse response;
            response.hold = true;
            return response;
        });
        m_server.on("GET", "/big", [](const HttpRequest &) { return HttpResponse{200, QByteArray(2 * 1024 * 1024, 'x'), "text/plain"}; });
        m_server.on("GET", "/jump", [](const HttpRequest &) {
            HttpResponse response{302, "moved"};
            response.headers = {"Location: /lookup"};
            return response;
        });
        m_server.on("GET", "/text", [](const HttpRequest &) { return HttpResponse{200, "plain words", "text/plain"}; });
    }

    void aJsonGetGivesTheStatusTheBodyAndPickedFields() {
        const Answer answer = call("/lookup", {{"path1", "data.items[1].name"}, {"path2", "data.count"}, {"path3", "$.data.owner"}});
        QVERIFY2(answer.ok, qPrintable(answer.message));
        QCOMPARE(answer.outputs.value("status").toInt(), 200);
        QVERIFY(answer.outputs.value("body").toString().contains("\"second\""));
        QCOMPARE(answer.outputs.value("field1").toString(), QString("second"));
        QCOMPARE(answer.outputs.value("field2").toString(), QString("3")); // a number as text
        QCOMPARE(answer.outputs.value("field3").toString(), QString("{\"id\":7}")); // an object as compact JSON
    }

    void thePathSubsetCoversKeysIndexesNegativeIndexesAndQuotedKeys() {
        const Answer answer = call("/lookup", {{"path1", "data.items[-1].name"}, {"path2", "data[\"ok\"]"}, {"path3", "data.items.0.name"}});
        QVERIFY2(answer.ok, qPrintable(answer.message));
        QCOMPARE(answer.outputs.value("field1").toString(), QString("second"));
        QCOMPARE(answer.outputs.value("field2").toString(), QString("true"));
        QCOMPARE(answer.outputs.value("field3").toString(), QString("first"));
    }

    void aPathThatIsMissingOrABodyThatIsNotJsonLeavesTheFieldOut() {
        Answer answer = call("/lookup", {{"path1", "data.nothing"}, {"path2", "data.items[9]"}});
        QVERIFY(answer.ok);
        QVERIFY(!answer.outputs.contains("field1") && !answer.outputs.contains("field2"));
        answer = call("/text", {{"path1", "a.b"}});
        QVERIFY(answer.ok);
        QCOMPARE(answer.outputs.value("body").toString(), QString("plain words"));
        QVERIFY(!answer.outputs.contains("field1"));
    }

    void aPathThatIsNotWellFormedIsABadInput() {
        const Answer answer = call("/lookup", {{"path1", "data..items"}});
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QString("bad_input"));
    }

    void aJsonPostFillsPlaceholdersAndKeepsTheBodyValidJson() {
        QJsonObject inputs = postJson("{\"text\": \"hello {a}\", \"n\": \"{b:json}\", \"who\": \"{a}\"}");
        inputs.insert("a", "Wo\"rld\nline"); // quotes and a line break must not break the JSON
        inputs.insert("b", "42");
        inputs.insert("headers", "X-Api-Key: made-up-{c}\n\nAccept: application/json");
        inputs.insert("c", "k1");
        const Answer answer = call("/hook", inputs);
        QVERIFY2(answer.ok, qPrintable(answer.message));
        QCOMPARE(answer.outputs.value("status").toInt(), 201);
        const QJsonObject echoed = QJsonDocument::fromJson(answer.outputs.value("body").toString().toUtf8()).object();
        QCOMPARE(echoed.value("type").toString(), QString("application/json"));
        QCOMPARE(echoed.value("key").toString(), QString("made-up-k1"));
        const QJsonObject sent = QJsonDocument::fromJson(echoed.value("got").toString().toUtf8()).object();
        QCOMPARE(sent.value("text").toString(), QString("hello Wo\"rld\nline"));
        QCOMPARE(sent.value("who").toString(), QString("Wo\"rld\nline"));
        QCOMPARE(sent.value("n").toInt(), 42); // {b:json} keeps the type
    }

    void aTextBodyIsSentAsItIsWithItsPlaceholdersFilled() {
        const Answer answer = call("/hook", {{"method", "POST"}, {"body_kind", "text"}, {"body", "note: {a}"}, {"a", "hi"}});
        QVERIFY2(answer.ok, qPrintable(answer.message));
        const QJsonObject echoed = QJsonDocument::fromJson(answer.outputs.value("body").toString().toUtf8()).object();
        QCOMPARE(echoed.value("got").toString(), QString("note: hi"));
        QVERIFY(echoed.value("type").toString().startsWith("text/plain"));
    }

    void aPlaceholderInTheUrlIsPercentEncoded() {
        m_server.on("GET", "/q%20%26x", [](const HttpRequest &) { return HttpResponse{200, "{\"hit\":true}"}; });
        const Answer answer = call("/{a}", {{"a", "q &x"}, {"path1", "hit"}});
        QVERIFY2(answer.ok, qPrintable(answer.message));
        QCOMPARE(answer.outputs.value("field1").toString(), QString("true"));
    }

    void aPictureGoesUpAsMultipartBesideTheFieldsOfTheBody() {
        const QString picture = m_dir.filePath("cat.png");
        QFile file(picture);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray("\x89PNG\r\n\x1a\n", 8) + "pixels-made-up");
        file.close();
        QJsonObject inputs = postJson("{\"caption\": \"a {a}\", \"n\": 3}");
        inputs.insert("a", "cat");
        inputs.insert("picture", QJsonObject{{"type", "image"}, {"path", picture}});
        inputs.insert("upload_field", "image");
        const int before = m_server.requestsTo("POST", "/upload").size();
        const Answer answer = call("/upload", inputs);
        QVERIFY2(answer.ok, qPrintable(answer.message));
        QCOMPARE(answer.outputs.value("status").toInt(), 200);
        const QList<HttpRequest> sent = m_server.requestsTo("POST", "/upload");
        QCOMPARE(sent.size(), before + 1);
        const HttpRequest &request = sent.last();
        QVERIFY(request.headers.value("content-type").startsWith("multipart/form-data; boundary="));
        QVERIFY(request.body.contains("name=\"caption\"\r\n\r\na cat\r\n"));
        QVERIFY(request.body.contains("name=\"n\"\r\n\r\n3\r\n"));
        QVERIFY(request.body.contains("name=\"image\"; filename=\"cat.png\"\r\nContent-Type: image/png\r\n\r\n"));
        QVERIFY(request.body.contains("pixels-made-up"));
    }

    void aStatusOfFourHundredIsAnOutputUnlessTheNodeIsToldToFail() {
        Answer answer = call("/missing");
        QVERIFY(answer.ok);
        QCOMPARE(answer.outputs.value("status").toInt(), 404);
        QVERIFY(answer.outputs.value("body").toString().contains("nope"));
        answer = call("/missing", {{"fail_on_error_status", true}});
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QString("http_status"));
    }

    void aRedirectIsNotFollowed() {
        const Answer answer = call("/jump");
        QVERIFY(answer.ok);
        QCOMPARE(answer.outputs.value("status").toInt(), 302);
        QCOMPARE(answer.outputs.value("body").toString(), QString("moved"));
    }

    void aServerThatDoesNotAnswerIsATimeout() {
        const Answer answer = call("/hold", {{"timeout_s", 1}});
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QString("node_timeout"));
        QVERIFY2(answer.message.contains("timeout_s"), qPrintable(answer.message)); // the hint says what to change
    }

    void anAnswerOverTheSizeCapIsRefusedAndOneUnderItPasses() {
        Answer answer = call("/big", {{"max_response_mb", 1}});
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QString("response_too_large"));
        answer = call("/big", {{"max_response_mb", 3}});
        QVERIFY(answer.ok);
        QCOMPARE(answer.outputs.value("body").toString().size(), qsizetype(2 * 1024 * 1024));
    }

    void plainHttpIsRefusedExceptOnTheLoopback() {
        const int before = m_server.requests().size();
        for (const QString url : {"http://example.com/x", "http://192.168.1.5/x", "http://[2001:db8::1]/x", "ftp://127.0.0.1/x"}) {
            const Answer answer = call({}, {{"url", url}});
            QVERIFY2(!answer.ok, qPrintable(url));
            QCOMPARE(answer.code, QString("bad_input"));
        }
        QCOMPARE(m_server.requests().size(), before); // refused before any connection
        QVERIFY(call("/text").ok); // http://127.0.0.1 is allowed
    }

    void aSecretIsNotSupportedYetInTheUrlTheHeadersOrTheBody() {
        const int before = m_server.requests().size();
        for (const char *field : {"headers", "body"}) {
            QJsonObject inputs{{"method", "POST"}, {"body_kind", "text"}, {"body", ""}, {"headers", ""}};
            inputs.insert(field, QString(field) == "headers" ? "Authorization: Bearer {secret:API_KEY}" : "key={secret:API_KEY}");
            const Answer answer = call("/hook", inputs);
            QVERIFY(!answer.ok);
            QCOMPARE(answer.code, QString("not_supported"));
            QVERIFY2(answer.message.contains("Secrets are not supported yet"), qPrintable(answer.message));
        }
        QVERIFY(!call("/x/{secret:API_KEY}").ok);
        QCOMPARE(m_server.requests().size(), before);
    }

    void aHeaderValueCannotCarryALineBreakInFromAnInput() {
        const Answer answer = call("/hook", {{"headers", "X-Note: {a}"}, {"a", "ok\r\nX-Evil: 1"}});
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QString("bad_input"));
    }

    void thePackDeclaresTheNetworkAndTheTrustPromptShowsIt() {
        if (QStandardPaths::findExecutable("script").isEmpty())
            QSKIP("script (util-linux) is needed to give the program a terminal");
        const QString command = shellQuote(FUGUE_BINARY) + " pack add " + shellQuote(kPack);
        const QByteArray shown = spawn("script", {"-qec", command, "/dev/null"}, "n\n", fugueEnv());
        QVERIFY2(shown.contains("network"), shown.constData());
        QVERIFY(!QFileInfo(m_dir.filePath("fugue-home/data/fugue/packs/io.github.bmaltais.fugue-http")).exists()); // a no installs nothing
    }

    void aValidationNeverCallsTheServerAndARunDoes() {
        if (QStandardPaths::findExecutable("script").isEmpty())
            QSKIP("script (util-linux) is needed to give the program a terminal");
        const QString command = shellQuote(FUGUE_BINARY) + " pack add " + shellQuote(kPack);
        spawn("script", {"-qec", command, "/dev/null"}, "y\n", fugueEnv());
        QVERIFY(QFileInfo(m_dir.filePath("fugue-home/data/fugue/packs/io.github.bmaltais.fugue-http/pack.json")).isFile());

        const QJsonObject inputs{{"method", "GET"}, {"url", QString(m_server.origin()) + "/lookup"}, {"path1", "data.items[0].name"}};
        const QJsonObject flow{{"fugue", 1},
                               {"graph", QJsonObject{{"nodes", QJsonObject{{"call", QJsonObject{{"type", kType}, {"version", "1.0.0"}, {"inputs", inputs}}}}}}}};
        QFile file(m_dir.filePath("call.fugue"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(json(flow));
        file.close();

        const int before = m_server.requestsTo("GET", "/lookup").size();
        int exitCode = -1;
        const QByteArray checked = fugue({"validate", file.fileName(), "--json"}, &exitCode);
        QCOMPARE(exitCode, 0);
        QVERIFY2(QJsonDocument::fromJson(checked).object().value("ok").toBool(), checked.constData());
        QCOMPARE(m_server.requestsTo("GET", "/lookup").size(), before); // checked, never called

        const QByteArray ran = fugue({"run", file.fileName(), "--wait", "--json"}, &exitCode);
        QCOMPARE(exitCode, 0);
        QCOMPARE(m_server.requestsTo("GET", "/lookup").size(), before + 1);
        QVERIFY2(ran.contains("run.done"), ran.constData());
    }
};

QTEST_GUILESS_MAIN(TestHttpRequestNode)
#include "test_http_request_node.moc"
