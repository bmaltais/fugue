// A tiny third-party provider for the tests of provider modules (spec 8.8): one program that
// speaks the provider protocol with no inference, over either transport.
//
//   fake_module_provider --stdio        JSON lines on stdin and stdout; status lines are pushed
//   fake_module_provider --http PORT    POST /fugue/v1/<op> and GET /fugue/v1/result/<job>/<i>
//
// It offers image.generate with one model, "toy" (a ready one), and a "toy-slow" that is loading.
// A job finishes at once with a 1x1 PNG. Over stdio it is written into the job's output_dir
// and the provider shares the filesystem; over http it is fetched. The job's params "fail": "<code>"
// ends it with that error. With FAKE_MODULE_LOG set it appends "start", "describe", "submit" and
// "shutdown" lines to that file.

#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTcpServer>
#include <QTcpSocket>

#include <cstdio>
#include <unistd.h>

namespace {

const QByteArray kPng = QByteArray::fromBase64(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");

void log(const QByteArray &text) {
    const QByteArray path = qgetenv("FAKE_MODULE_LOG");
    if (path.isEmpty())
        return;
    QFile file(QString::fromLocal8Bit(path));
    if (file.open(QIODevice::Append))
        file.write(text + '\n');
}

QJsonObject describeReply(bool shares) {
    const QJsonObject toy{{"name", "toy"}, {"title", "Toy model"}, {"status", "ready"}, {"properties", QJsonObject{{"edit", false}}}};
    const QJsonObject slow{{"name", "toy-slow"}, {"status", "loading"}, {"reason", "It is warming up."}};
    return {{"protocol", 1},
            {"engine", "toy-engine"},
            {"shares_filesystem", shares},
            {"capabilities",
             QJsonObject{{"image.generate",
                          QJsonObject{{"models", QJsonArray{toy, slow}},
                                      {"defaults", QJsonObject{{"steps", 2}}},
                                      {"limits", QJsonObject{{"max_width", 64}}}}}}}};
}

QJsonObject failure(const QString &code, const QString &message) {
    return {{"error", QJsonObject{{"code", code}, {"message", message}}}};
}

struct Job {
    QJsonObject request;
    bool shares = false;
};

// What the two transports share: the state of the jobs and the answer to each operation.
class Engine {
public:
    explicit Engine(bool shares) : m_shares(shares) {}

    // The reply to `message`, and the status lines to push after it (stdio).
    QJsonObject handle(const QJsonObject &message, QList<QJsonObject> *pushed = nullptr) {
        const QString op = message.value("op").toString();
        if (op == QLatin1String("describe")) {
            log("describe");
            return describeReply(m_shares);
        }
        if (op == QLatin1String("submit")) {
            log("submit " + message.value("model").toObject().value("name").toString().toUtf8());
            const QString id = QString::number(++m_count);
            m_jobs.insert(id, message);
            if (pushed) {
                pushed->append({{"op", "status"}, {"job", id}, {"state", "running"}, {"progress", 0.5}, {"stage", "sample"}});
                pushed->append(finish(id));
            }
            return {{"job", id}};
        }
        if (op == QLatin1String("shutdown")) {
            log("shutdown");
            m_stop = true;
            return {};
        }
        const QString id = message.value("job").toString();
        if (!m_jobs.contains(id))
            return failure("input_invalid", "no such job");
        if (op == QLatin1String("status")) {
            const QJsonObject over = finish(id);
            QJsonObject reply{{"state", over.value("state")}, {"progress", 1.0}};
            if (over.contains("error"))
                reply.insert("error", over.value("error"));
            return reply;
        }
        if (op == QLatin1String("cancel"))
            return {{"state", "cancelled"}};
        if (op == QLatin1String("result")) {
            const QJsonObject request = m_jobs.value(id);
            const QJsonObject used{{"seed", request.value("params").toObject().value("seed")}};
            if (m_shares) {
                const QString path = request.value("output_dir").toString() + "/image.png";
                QFile file(path);
                if (!file.open(QIODevice::WriteOnly))
                    return failure("provider_failed", "cannot write " + path);
                file.write(kPng);
                return {{"outputs", QJsonObject{{"image", QJsonObject{{"path", path}, {"format", "png"}}}}}, {"used", used}};
            }
            return {{"outputs", QJsonObject{{"image", QJsonObject{{"file", 0}, {"format", "png"}, {"size", kPng.size()}}}}},
                    {"used", used}};
        }
        return failure("input_invalid", "unknown op " + op);
    }

    bool stopping() const { return m_stop; }

private:
    QJsonObject finish(const QString &id) const {
        const QString code = m_jobs.value(id).value("params").toObject().value("fail").toString();
        QJsonObject status{{"op", "status"}, {"job", id}, {"state", code.isEmpty() ? "done" : "error"}};
        if (!code.isEmpty())
            status.insert("error", QJsonObject{{"code", code}, {"message", "the toy failed as asked"}});
        return status;
    }

    bool m_shares;
    bool m_stop = false;
    int m_count = 0;
    QHash<QString, QJsonObject> m_jobs;
};

class Stdio : public QObject {
public:
    Stdio() : m_engine(true) {
        m_input = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
        connect(m_input, &QSocketNotifier::activated, this, [this] { read(); });
    }

private:
    static void say(const QJsonObject &message) {
        const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
        std::fflush(stdout);
    }

    void read() {
        char buffer[4096];
        const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (got <= 0) {
            QCoreApplication::exit(0);
            return;
        }
        m_in.append(buffer, got);
        qsizetype end;
        while ((end = m_in.indexOf('\n')) >= 0) {
            const QJsonObject message = QJsonDocument::fromJson(m_in.left(end)).object();
            m_in.remove(0, end + 1);
            QList<QJsonObject> pushed;
            QJsonObject reply = m_engine.handle(message, &pushed);
            reply.insert("id", message.value("id"));
            say(reply);
            for (const QJsonObject &status : std::as_const(pushed))
                say(status);
            if (m_engine.stopping())
                QCoreApplication::exit(0);
        }
    }

    Engine m_engine;
    QSocketNotifier *m_input;
    QByteArray m_in;
};

class Http : public QObject {
public:
    explicit Http(quint16 port) : m_engine(false) {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        if (!m_server.listen(QHostAddress::LocalHost, port))
            std::fputs("cannot listen\n", stderr);
    }

private:
    void read(QTcpSocket *socket) {
        QByteArray &data = m_pending[socket];
        data += socket->readAll();
        const qsizetype headerEnd = data.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray head = data.left(headerEnd);
        qsizetype length = 0;
        for (const QByteArray &line : head.split('\n'))
            if (line.toLower().startsWith("content-length:"))
                length = line.mid(15).trimmed().toLongLong();
        if (data.size() < headerEnd + 4 + length)
            return;
        const QByteArray body = data.mid(headerEnd + 4, length);
        const QList<QByteArray> request = head.left(head.indexOf('\r')).split(' ');
        m_pending.remove(socket);
        const QString path = QString::fromLatin1(request.value(1));
        QByteArray reply;
        QByteArray type = "application/json";
        if (request.value(0) == "GET" && path.startsWith("/fugue/v1/result/")) {
            reply = kPng;
            type = "image/png";
        } else {
            const QString op = path.section('/', 3);
            QJsonObject message = QJsonDocument::fromJson(body).object();
            message.insert("op", op);
            reply = QJsonDocument(m_engine.handle(message)).toJson(QJsonDocument::Compact);
        }
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " +
                      QByteArray::number(reply.size()) + "\r\nConnection: close\r\n\r\n" + reply);
        socket->disconnectFromHost();
        if (m_engine.stopping())
            QCoreApplication::exit(0);
    }

    QTcpServer m_server;
    Engine m_engine;
    QHash<QTcpSocket *, QByteArray> m_pending;
};

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    log("start");
    if (arguments.value(1) == QLatin1String("--http") && arguments.size() > 2) {
        Http http(arguments.at(2).toUShort());
        return app.exec();
    }
    if (arguments.value(1) == QLatin1String("--stdio")) {
        Stdio stdio;
        return app.exec();
    }
    std::fputs("usage: fake_module_provider --stdio | --http PORT\n", stderr);
    return 2;
}
