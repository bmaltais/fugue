#include "engine/remote_request.h"

#include <QHostAddress>
#include <QHttpMultiPart>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>


namespace fugue {

bool isLocalHost(const QString &host) {
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 || host.endsWith(QLatin1String(".localhost"), Qt::CaseInsensitive) ||
        host.endsWith(QLatin1String(".local"), Qt::CaseInsensitive) || host.endsWith(QLatin1String(".internal"), Qt::CaseInsensitive))
        return true;
    QHostAddress address;
    if (!address.setAddress(host))
        return false;
    return address.isLoopback() || address.isLinkLocal() || address.isPrivateUse() || address.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7) || address.isNull() ||
           address == QHostAddress::Any || address == QHostAddress::AnyIPv4 || address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10);
}

bool downloadUrlAllowed(const QUrl &url, const QString &instanceHost) {
    if (!remote::wholeUrl(url))
        return false;
    return !isLocalHost(url.host()) || isLocalHost(instanceHost);
}

namespace remote {

namespace {
const char *const kCapped = "fugue-capped";

bool atOrigin(const Session &session, const QUrl &url) {
    const QString origin = originOf(url);
    return std::any_of(session.origins.begin(), session.origins.end(), [&](const QUrl &own) { return wholeUrl(url) && originOf(own) == origin; });
}
} // namespace

QNetworkReply *send(QNetworkAccessManager &network, const Session &session, const QUrl &url, const Call &call) {
    QNetworkRequest request(url);
    request.setTransferTimeout(session.transferTimeoutMs);
    // No redirect is followed by Qt: a download's is checked hop by hop, and an API call's is an error.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (call.withKey && !session.key.isEmpty() && (call.keyAnywhere || atOrigin(session, url)))
        request.setRawHeader(session.header, (session.scheme.isEmpty() ? QByteArray() : session.scheme + ' ') + session.key.toUtf8());
    for (const auto &header : call.headers)
        request.setRawHeader(header.first, header.second);
    if (!call.form && call.method != "GET")
        request.setHeader(QNetworkRequest::ContentTypeHeader, call.contentType);
    QNetworkReply *reply = call.form ? network.sendCustomRequest(request, call.method, call.form) : network.sendCustomRequest(request, call.method, call.body);
    if (call.form)
        call.form->setParent(reply);
    const qint64 cap = call.cap;
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply, cap](qint64 got, qint64) {
        if (got > cap && !reply->property(kCapped).toBool()) {
            reply->setProperty(kCapped, true);
            reply->abort();
        }
    });
    return reply;
}

bool wasCapped(QNetworkReply *reply) { return reply->property(kCapped).toBool(); }

int statusOf(QNetworkReply *reply) { return wasCapped(reply) ? 0 : reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); }

bool isRedirect(int code) { return code >= 300 && code < 400 && code != 304; }

QUrl redirectTarget(QNetworkReply *reply) { return reply->url().resolved(QUrl(QString::fromLatin1(reply->rawHeader("Location")))); }

bool followHop(int &hops) { return ++hops <= kMaxHops; }

bool transientStatus(int code, bool serverError) { return code == 0 || code == 429 || (serverError && code == 500) || code == 502 || code == 503 || code == 504; }

bool pollsExhausted(int &failures) { return ++failures >= kMaxTransientPolls; }

int retryAfterMs(QNetworkReply *reply) {
    bool seconds = false;
    const int wait = QString::fromLatin1(reply->rawHeader("Retry-After")).trimmed().toInt(&seconds);
    return seconds ? std::clamp(wait, 0, 30) * 1000 : 0;
}

int jobDeadlineMs(int timeoutMs) { return timeoutMs * kJobDeadlineFactor; }

bool wholeUrl(const QUrl &url) {
    return (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) && !url.host().isEmpty();
}

QString originOf(const QUrl &url) {
    return QStringLiteral("%1://%2:%3").arg(url.scheme(), url.host().toLower()).arg(url.port(url.scheme() == QLatin1String("https") ? 443 : 80));
}

bool loopbackHost(const QString &host) {
    QHostAddress address;
    return host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 || host.endsWith(QLatin1String(".localhost"), Qt::CaseInsensitive) ||
           (address.setAddress(host) && address.isLoopback());
}

bool keyTransportOk(const QUrl &url) { return url.scheme() == QLatin1String("https") || loopbackHost(url.host()); }

} // namespace remote
} // namespace fugue
