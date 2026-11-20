#pragma once

// A minimal HTTP/1.1 server for provider tests. It listens on a free loopback port, answers each
// request from a handler keyed by method and path, and records what it was sent. One request per
// connection; every answer closes the connection. Never point a test at a real server.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>
#include <memory>

namespace fugue::test {

struct HttpRequest {
    QByteArray method;
    QByteArray path;
    QHash<QByteArray, QByteArray> headers; // names in lower case
    QByteArray body;
};

struct HttpResponse {
    int status = 200;
    QByteArray body;
    QByteArray contentType = "application/json";
    // Never answered: the connection stays open with nothing sent, as a server that is still
    // working on the request.
    bool hold = false;
};

class FakeHttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;

    FakeHttpServer() {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection())
                accept(socket);
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    // Stops listening, so new connections are refused, as by a server that died.
    void close() { m_server.close(); }
    quint16 port() const { return m_server.serverPort(); }
    QByteArray origin() const { return "http://127.0.0.1:" + QByteArray::number(port()); }

    void on(const QByteArray &method, const QByteArray &path, Handler handler) {
        m_handlers.insert(method + ' ' + path, std::move(handler));
    }

    const QList<HttpRequest> &requests() const { return m_requests; }
    QList<HttpRequest> requestsTo(const QByteArray &method, const QByteArray &path) const {
        QList<HttpRequest> found;
        for (const HttpRequest &request : m_requests)
            if (request.method == method && request.path == path)
                found.append(request);
        return found;
    }

private:
    void accept(QTcpSocket *socket) {
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
            buffer->append(socket->readAll());
            const qsizetype end = buffer->indexOf("\r\n\r\n");
            if (end < 0)
                return;
            HttpRequest request;
            const QList<QByteArray> lines = buffer->left(end).split('\n');
            const QList<QByteArray> start = lines.first().trimmed().split(' ');
            request.method = start.value(0);
            request.path = start.value(1);
            for (qsizetype i = 1; i < lines.size(); ++i) {
                const qsizetype colon = lines.at(i).indexOf(':');
                if (colon > 0)
                    request.headers.insert(lines.at(i).left(colon).trimmed().toLower(),
                                           lines.at(i).mid(colon + 1).trimmed());
            }
            const qsizetype length = request.headers.value("content-length", "0").toLongLong();
            if (buffer->size() < end + 4 + length)
                return;
            request.body = buffer->mid(end + 4, length);
            buffer->clear();
            m_requests.append(request);
            const auto handler = m_handlers.constFind(request.method + ' ' + request.path);
            const HttpResponse response = handler == m_handlers.constEnd()
                                              ? HttpResponse{404, "{\"error\":\"not found\"}"}
                                              : (*handler)(request);
            if (response.hold)
                return;
            QByteArray reply = "HTTP/1.1 " + QByteArray::number(response.status) + " X\r\n" +
                               "Content-Type: " + response.contentType + "\r\n" +
                               "Content-Length: " + QByteArray::number(response.body.size()) +
                               "\r\nConnection: close\r\n\r\n" + response.body;
            socket->write(reply);
            socket->disconnectFromHost();
        });
    }

    QTcpServer m_server;
    QHash<QByteArray, Handler> m_handlers;
    QList<HttpRequest> m_requests;
};

} // namespace fugue::test
