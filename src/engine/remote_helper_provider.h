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
#include <QTimer>

#include <functional>

#include "engine/providers.h"

class QNetworkReply;

namespace fugue {

class RemoteHelperProvider : public Provider {
    Q_OBJECT

public:
    explicit RemoteHelperProvider(const ProviderInstance &instance, int pollMs = 500, QObject *parent = nullptr);
    ~RemoteHelperProvider() override;

    void describe(Reply<Description> reply) override;
    void submit(const JobRequest &request, Reply<QString> reply) override;
    void status(const QString &job, Reply<JobStatus> reply) override;
    void cancel(const QString &job, Reply<QString> reply) override;
    void result(const QString &job, Reply<JobResult> reply) override;

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

    void post(const QString &op, const QJsonObject &body, Answer answer);
    QNetworkReply *request(const QString &method, const QString &path, const QByteArray &body);
    Error unreachable(const QString &why) const;
    // The reply as JSON, or why it is not.
    Result<QJsonObject> parse(QNetworkReply *reply) const;
    void poll();
    void fetch(const QString &job);
    void change(const QString &job, const JobStatus &status);
    void fail(const QString &job, const Error &error);

    ProviderInstance m_instance;
    QNetworkAccessManager m_network;
    QTimer m_poll;
    QHash<QString, Job> m_jobs;
};

} // namespace fugue
