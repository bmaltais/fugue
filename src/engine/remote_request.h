#pragma once

// Remote request safety: the rules every provider that talks to a service over the network keeps, in one place
// (#582). A provider builds its request (URL, body, answer parsing, error mapping) and sends it through
// remote::send(); what that guarantees:
//   - the key goes only to the origins the provider names (and, by an explicit flag, to one more), never to a
//     URL a service named, and is withheld when no key is set;
//   - no redirect is followed by Qt: the caller sees the 3xx and decides (remote::isRedirect, followHop);
//   - an answer is cut off past a size: the reply is aborted and reads as status 0 (remote::wasCapped);
//   - one transfer timeout per request (a job's own deadline is remote::jobDeadlineMs).
// The rest are the questions a provider asks before it sends: may the key travel to this URL (keyTransportOk),
// may a link the service named be fetched (downloadUrlAllowed), is a status worth another poll (transientStatus).
// isLocalHost() is the consent rule (remote_consent.h): whether a host is the person's own network. It is a
// different question from loopbackHost() (the key's transport) and stays a separate function.

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

class QHttpMultiPart;
class QNetworkAccessManager;
class QNetworkReply;

namespace fugue {

// Whether a host is on this machine or its own network: localhost, *.local and *.internal names, loopback, link-local,
// private (RFC 1918, unique local) and CGNAT (100.64/10, Tailscale) addresses, the unspecified address. A host that is not is
// off-machine (remote_consent.h).
bool isLocalHost(const QString &host);

// Whether a link a service names may be fetched: http or https, and not a loopback, link-local or private
// address unless the instance itself (`instanceHost`) is at one.
bool downloadUrlAllowed(const QUrl &url, const QString &instanceHost);

namespace remote {

constexpr int kMaxHops = 3;           // redirects followed by one download
constexpr int kMaxTransientPolls = 5; // polls in a row that may fail before the job does
constexpr int kJobDeadlineFactor = 6; // a job gives up after this many transfer timeouts

// How a provider's key is sent: `header: scheme key` (scheme empty: the key alone), only to URLs at `origins`.
struct Session {
    QByteArray header = "Authorization";
    QByteArray scheme;
    QString key;
    QList<QUrl> origins;
    int transferTimeoutMs = 0; // 0: none (the caller has a deadline of its own)
};

struct Call {
    QByteArray method = "GET";
    QByteArray body;
    QByteArray contentType;
    QHttpMultiPart *form = nullptr; // sent instead of `body`; the reply takes it over
    qint64 cap = 64LL << 20;        // the answer is cut off past this many bytes
    bool withKey = false;           // the key goes along if the URL is at one of the session's origins
    bool keyAnywhere = false;       // ... or at any URL (the caller has judged it)
    QList<QPair<QByteArray, QByteArray>> headers;
};

// Sends the call under the rules above.
QNetworkReply *send(QNetworkAccessManager &network, const Session &session, const QUrl &url, const Call &call);

// The status of a reply: 0 when nothing answered or the answer was cut off for its size.
int statusOf(QNetworkReply *reply);
bool wasCapped(QNetworkReply *reply);

// A redirect to follow by hand (not 304, which has no Location). The target resolved against the reply's URL.
bool isRedirect(int code);
QUrl redirectTarget(QNetworkReply *reply);
// Counts one more redirect in `hops`; false once the limit is passed.
bool followHop(int &hops);

// A poll answer worth trying again: nothing, 429, 502-504, and 500 unless `serverError` is false.
bool transientStatus(int code, bool serverError = true);
// Counts one more failed poll in `failures`; true once the budget is spent.
bool pollsExhausted(int &failures);
// The wait a Retry-After header asks for (seconds, at most 30), in ms; 0 when there is none.
int retryAfterMs(QNetworkReply *reply);
int jobDeadlineMs(int timeoutMs);

// Whether a URL is http or https with a host.
bool wholeUrl(const QUrl &url);
// "scheme://host:port", lower case, the default port made explicit.
QString originOf(const QUrl &url);
// localhost, *.localhost and loopback addresses.
bool loopbackHost(const QString &host);
// Whether a key may be sent to this URL: https, or a loopback host (a fake in a test, a service on this machine).
bool keyTransportOk(const QUrl &url);

} // namespace remote
} // namespace fugue
