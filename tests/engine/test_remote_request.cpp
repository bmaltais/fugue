// The request-safety rules every remote provider shares (#582), proven once here against local fake servers.
// Each provider's own tests keep its request shape and error mapping.

#include <QHttpMultiPart>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QtTest>

#include "engine/remote_request.h"
#include "fake_http.h"
#include "../test_shard.h"

using namespace fugue;
using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

// Sends a call and waits for the reply to finish; the reply is deleted with the object it is made a child of.
class Sent {
public:
    Sent(QNetworkAccessManager &network, const remote::Session &session, const QUrl &url, const remote::Call &call) {
        reply = remote::send(network, session, url, call);
        reply->setParent(&holder);
        QVERIFY2(QTest::qWaitFor([&] { return reply->isFinished(); }, 5000), "no reply");
    }
    QObject holder;
    QNetworkReply *reply = nullptr;
};

QUrl at(const FakeHttpServer &server, const char *path) { return QUrl(QString::fromLatin1(server.origin() + path)); }

remote::Call keyed() {
    remote::Call call;
    call.withKey = true;
    return call;
}

} // namespace

class TestRemoteRequest : public QObject {
    Q_OBJECT

    QNetworkAccessManager m_network;
    FakeHttpServer m_own, m_other;

private slots:
    void initTestCase() {
        QVERIFY(m_own.listen());
        QVERIFY(m_other.listen());
        for (FakeHttpServer *server : {&m_own, &m_other}) {
            server->on("GET", "/ok", [](const HttpRequest &) { return HttpResponse{200, "{}"}; });
            server->on("POST", "/ok", [](const HttpRequest &) { return HttpResponse{200, "{}"}; });
        }
    }
    void init() {
        m_own.clearRequests();
        m_other.clearRequests();
    }

    void theKeyGoesOnlyToTheOriginsItIsGivenAndInTheSchemeAsked() {
        remote::Session session;
        session.key = "k-secret";
        session.scheme = "Key";
        session.origins = {QUrl(QString::fromLatin1(m_own.origin()))};
        Sent own(m_network, session, at(m_own, "/ok"), keyed());
        QCOMPARE(m_own.requests().last().headers.value("authorization"), QByteArray("Key k-secret"));
        // Another origin, even on the same host: another port.
        Sent other(m_network, session, at(m_other, "/ok"), keyed());
        QVERIFY(!m_other.requests().last().headers.contains("authorization"));
        // Unless the caller has judged that URL itself.
        remote::Call anywhere = keyed();
        anywhere.keyAnywhere = true;
        Sent judged(m_network, session, at(m_other, "/ok"), anywhere);
        QCOMPARE(m_other.requests().last().headers.value("authorization"), QByteArray("Key k-secret"));
        // A call that does not ask for the key never sends it; a session without a key sends no header.
        Sent bare(m_network, session, at(m_own, "/ok"), remote::Call{});
        QVERIFY(!m_own.requests().last().headers.contains("authorization"));
        remote::Session keyless = session;
        keyless.key.clear();
        Sent none(m_network, keyless, at(m_own, "/ok"), keyed());
        QVERIFY(!m_own.requests().last().headers.contains("authorization"));
    }
    void aKeyCanTravelInAnotherHeaderWithoutAScheme() {
        remote::Session session;
        session.key = "g-secret";
        session.header = "x-goog-api-key";
        session.origins = {QUrl(QString::fromLatin1(m_own.origin()))};
        Sent sent(m_network, session, at(m_own, "/ok"), keyed());
        QCOMPARE(m_own.requests().last().headers.value("x-goog-api-key"), QByteArray("g-secret"));
    }
    void aRedirectIsNotFollowedAndItsTargetIsNeverAsked() {
        remote::Session session;
        session.key = "k-secret";
        session.origins = {QUrl(QString::fromLatin1(m_own.origin()))};
        m_own.on("GET", "/moved", [this](const HttpRequest &) {
            return HttpResponse{302, {}, "text/plain", false, -1, {"Location: " + m_other.origin() + "/ok"}};
        });
        Sent sent(m_network, session, at(m_own, "/moved"), keyed());
        QCOMPARE(remote::statusOf(sent.reply), 302);
        QVERIFY(remote::isRedirect(302));
        QCOMPARE(remote::redirectTarget(sent.reply), at(m_other, "/ok"));
        QVERIFY(m_other.requests().isEmpty());
    }
    void anAnswerPastItsCapIsCutOffAndReadsAsNothing() {
        m_own.on("GET", "/big", [](const HttpRequest &) { return HttpResponse{200, QByteArray(300000, 'x'), "application/octet-stream"}; });
        remote::Call call;
        call.cap = 1000;
        Sent big(m_network, {}, at(m_own, "/big"), call);
        QVERIFY(remote::wasCapped(big.reply));
        QCOMPARE(remote::statusOf(big.reply), 0);
        // Within the cap it is an ordinary 200.
        Sent fine(m_network, {}, at(m_own, "/ok"), call);
        QVERIFY(!remote::wasCapped(fine.reply));
        QCOMPARE(remote::statusOf(fine.reply), 200);
    }
    void aTransferThatSaysNothingEndsAtTheTimeout() {
        m_own.on("GET", "/hold", [](const HttpRequest &) { return HttpResponse{200, {}, "text/plain", true}; });
        remote::Session session;
        session.transferTimeoutMs = 200;
        Sent slow(m_network, session, at(m_own, "/hold"), remote::Call{});
        QCOMPARE(slow.reply->error(), QNetworkReply::TimeoutError);
        QCOMPARE(remote::statusOf(slow.reply), 0);
    }
    void aFormIsSentWithItsOwnTypeAndKey() {
        remote::Session session;
        session.key = "k-secret";
        session.scheme = "Bearer";
        session.origins = {QUrl(QString::fromLatin1(m_own.origin()))};
        auto *form = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"a\""));
        part.setBody("1");
        form->append(part);
        remote::Call call = keyed();
        call.method = "POST";
        call.form = form;
        Sent sent(m_network, session, at(m_own, "/ok"), call);
        const HttpRequest request = m_own.requests().last();
        QCOMPARE(remote::statusOf(sent.reply), 200);
        QVERIFY(request.headers.value("content-type").startsWith("multipart/form-data; boundary="));
        QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer k-secret"));
        QVERIFY(request.body.contains("name=\"a\""));
    }
    void aKeyTravelsOnlyOverHttpsOrToThisMachine() {
        QVERIFY(remote::keyTransportOk(QUrl("https://api.example.com")));
        QVERIFY(!remote::keyTransportOk(QUrl("http://api.example.com")));
        QVERIFY(!remote::keyTransportOk(QUrl("http://192.168.1.5"))); // own network is not this machine
        for (const char *url : {"http://localhost:1", "http://app.localhost", "http://127.0.0.1:9", "http://[::1]:9"})
            QVERIFY2(remote::keyTransportOk(QUrl(url)), url);
        QVERIFY(!remote::loopbackHost(QStringLiteral("localhost.example.com")));
    }
    void aLinkAServiceNamesIsFetchedOnlyOverHttpAndNotFromAnotherMachinesNetwork() {
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
    void redirectsAndPollFailuresAreBudgeted() {
        int hops = 0;
        for (int i = 0; i < remote::kMaxHops; ++i)
            QVERIFY(remote::followHop(hops));
        QVERIFY(!remote::followHop(hops));
        int failures = 0;
        for (int i = 1; i < remote::kMaxTransientPolls; ++i)
            QVERIFY(!remote::pollsExhausted(failures));
        QVERIFY(remote::pollsExhausted(failures));
        for (int code : {0, 429, 500, 502, 503, 504})
            QVERIFY(remote::transientStatus(code));
        QVERIFY(!remote::transientStatus(500, false));
        for (int code : {200, 202, 400, 401, 404, 501})
            QVERIFY(!remote::transientStatus(code));
        QVERIFY(!remote::isRedirect(304));
        QVERIFY(!remote::isRedirect(200));
        QCOMPARE(remote::jobDeadlineMs(1000), 6000);
    }
    void aRetryAfterIsReadInSecondsAndKeptUnderHalfAMinute() {
        m_own.on("GET", "/r", [](const HttpRequest &) { return HttpResponse{429, {}, "text/plain", false, -1, {"Retry-After: 2"}}; });
        m_own.on("GET", "/r-long", [](const HttpRequest &) { return HttpResponse{429, {}, "text/plain", false, -1, {"Retry-After: 3600"}}; });
        m_own.on("GET", "/r-date", [](const HttpRequest &) { return HttpResponse{429, {}, "text/plain", false, -1, {"Retry-After: Wed, 21 Oct 2026 07:28:00 GMT"}}; });
        QCOMPARE(remote::retryAfterMs(Sent(m_network, {}, at(m_own, "/r"), {}).reply), 2000);
        QCOMPARE(remote::retryAfterMs(Sent(m_network, {}, at(m_own, "/r-long"), {}).reply), 30000);
        QCOMPARE(remote::retryAfterMs(Sent(m_network, {}, at(m_own, "/r-date"), {}).reply), 0);
    }
    void originsIgnoreCaseAndMakeDefaultPortsExplicit() {
        QCOMPARE(remote::originOf(QUrl("HTTPS://API.Example.com/x")), QStringLiteral("https://api.example.com:443"));
        QCOMPARE(remote::originOf(QUrl("http://a.test")), QStringLiteral("http://a.test:80"));
        QVERIFY(remote::wholeUrl(QUrl("http://a.test")));
        QVERIFY(!remote::wholeUrl(QUrl("ftp://a.test")));
        QVERIFY(!remote::wholeUrl(QUrl("https:///nohost")));
    }
};

FUGUE_TEST_MAIN(TestRemoteRequest, QCoreApplication)
#include "test_remote_request.moc"
