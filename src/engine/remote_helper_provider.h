#pragma once

// The fugue-helper provider on a paired host (spec 8.8 `http` transport, 8.10): the same stage
// capabilities as HelperProvider, served by a helper running on another machine, e.g. the DGX
// Spark. Every operation is `POST <address>/fugue/v1/<op>` with JSON in and out and
// `Authorization: Bearer <key>`.
//
// - The host does not share this machine's filesystem (describe says shares_filesystem false):
//   the files of a job's `inputs` ({"path": p}: stage files and images) go as
//   {"data": base64, "format"}, per job; its models are handles ({"name","sha256","size"}) the
//   host resolves in its own folders (the stage nodes make them, nodes/components.cpp).
// - There is no stream: status is polled, and statusChanged is emitted as it changes. The result's
//   files are downloaded with `GET <address>/fugue/v1/result/<job>/<i>` into the job's outputDir,
//   so the JobResult has {"path", "format"} entries exactly as the local helper's.
// - The helper is started by whoever runs the host, never by Fugue: nothing here starts or stops
//   a process, and shutdown is never sent.
// - Errors: out_of_memory, cancelled and model_missing keep their codes; an unreachable host or a
//   rejected key is provider_unreachable; anything else is node_failed.

#include <QHash>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <functional>

#include "engine/providers.h"

class QNetworkReply;

namespace fugue {

// What makes this the provider of an http module (spec 8.8) rather than the helper of a paired
// host. The defaults are the paired helper's.
struct RemoteProviderOptions {
    QString where = QStringLiteral("the paired host"); // in messages
    QString filesTitle = QStringLiteral("Model files on the paired host");
    QString defaultEngine = QStringLiteral("sdcpp");
    // What describe offers when its reply names no capabilities at all.
    QStringList defaultCapabilities;

    // A module Fugue starts: the program and its arguments ({port} in them and in
    // `addressTemplate` is a free loopback port), started by the first describe or submit, asked
    // to shut down and ended after `idleMs` without a job (0: never), and given `startTimeoutMs`
    // to answer describe. Used only when the instance has no address of its own.
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QString addressTemplate;
    int idleMs = 0;
    int startTimeoutMs = 30000;
};

class RemoteHelperProvider : public Provider {
    Q_OBJECT

public:
    explicit RemoteHelperProvider(const ProviderInstance &instance, int pollMs = 500, QObject *parent = nullptr);
    RemoteHelperProvider(const ProviderInstance &instance, RemoteProviderOptions options, int pollMs = 500,
                         QObject *parent = nullptr);
    ~RemoteHelperProvider() override;

    void describe(Reply<Description> reply) override;
    void submit(const JobRequest &request, Reply<QString> reply) override;
    void status(const QString &job, Reply<JobStatus> reply) override;
    void cancel(const QString &job, Reply<QString> reply) override;
    void result(const QString &job, Reply<JobResult> reply) override;
    // The models under the host's --models roots (`POST /fugue/v1/models`, 8.7), as the host answered. `hash`: ask it to hash, in
    // the background, up to a few files that have no hash yet, `names` first. A host without the operation is the error
    // `not_found`. Read-only on the host.
    void listModels(bool hash, const QStringList &names, Reply<QJsonObject> reply);
    // Only for a provider Fugue started: asks it to shut down and ends it; returns once it is gone.
    void shutdown(Reply<Nothing> reply) override;

private:
    struct Job {
        QString hostJob;
        QString outputDir;
        JobStatus status;
        JobResult result;
        bool polling = false;   // a status request is out, or the result is being fetched
        int failures = 0;       // polls in a row that did not reach the host
    };

    using Answer = std::function<void(const Result<QJsonObject> &)>;

    // `mayStart`: describe and submit start a module that is not running; the other operations
    // belong to a job of the running one, which has died if it is not (its jobs then fail).
    void post(const QString &op, const QJsonObject &body, Answer answer, bool mayStart = false);
    void sendPost(const QString &op, const QJsonObject &body, Answer answer);
    QString address() const;
    bool launched() const { return !m_options.program.isEmpty() && m_instance.address.isEmpty(); }
    using Ready = std::function<void(const QString &why)>;
    void whenUp(Ready ready);
    void probe();
    void startDone(const QString &why);
    void touch();
    // Ends the process: shutdown asked, terminated, killed after the grace period. Returns it
    // while it still runs.
    QProcess *stopServer();
    QNetworkReply *request(const QString &method, const QString &path, const QByteArray &body);
    Error unreachable(const QString &why) const;
    // The reply as JSON, or why it is not.
    Result<QJsonObject> parse(QNetworkReply *reply) const;
    void poll();
    void fetch(const QString &job);
    void change(const QString &job, const JobStatus &status);
    void fail(const QString &job, const Error &error);

    ProviderInstance m_instance;
    RemoteProviderOptions m_options;
    QNetworkAccessManager m_network;
    QTimer m_poll;
    QHash<QString, Job> m_jobs;

    // The module's process, while Fugue runs one (launched()).
    QProcess *m_process = nullptr;
    bool m_up = false;       // it answered describe
    bool m_starting = false;
    QString m_runAddress;    // addressTemplate with its port
    QList<Ready> m_waiters;  // operations that wait for it to be up
    QStringList m_log;       // its last output lines
    QTimer m_idle;
    QTimer m_startLimit;
};

} // namespace fugue
