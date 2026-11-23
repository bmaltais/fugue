#pragma once

// A provider for tests (spec 8.11): it serves a fixed description, and its jobs move only when
// the test says so. Results are written to the request's outputDir, as a real provider does.

#include <QHash>
#include <QMap>

#include "engine/provider.h"

namespace fugue {

class FakeProvider : public Provider {
    Q_OBJECT

public:
    explicit FakeProvider(Description description, QObject *parent = nullptr);

    void setDescription(const Description &description);
    // describe() fails with these until cleared, as an unreachable provider does.
    void setDescribeErrors(const Errors &errors);

    void describe(Reply<Description> reply) override;
    void submit(const JobRequest &request, Reply<QString> reply) override;
    void status(const QString &job, Reply<JobStatus> reply) override;
    void cancel(const QString &job, Reply<QString> reply) override;
    void result(const QString &job, Reply<JobResult> reply) override;
    void shutdown(Reply<Nothing> reply) override;

    // What the engine sent for a job, and every job id in submission order.
    JobRequest submitted(const QString &job) const;
    QStringList jobs() const;
    // How many times shutdown() was called.
    int shutdowns() const { return m_shutdowns; }

    // Moves a job, emitting statusChanged.
    void progress(const QString &job, double value, const QString &stage);
    // Reports a running job with a preview frame (PNG bytes).
    void preview(const QString &job, const QByteArray &frame);
    // Writes each file (name -> bytes) into outputDir; each becomes an output named by the
    // file's base name, e.g. image.png -> outputs.image.
    void finish(const QString &job, const QMap<QString, QByteArray> &files,
                const QJsonObject &used = {});
    void fail(const QString &job, const Error &error);

private:
    struct Job {
        JobRequest request;
        JobStatus status;
        JobResult result;
    };

    void change(const QString &job, const JobStatus &status);

    Description m_description;
    Errors m_describeErrors;
    QHash<QString, Job> m_jobs;
    QStringList m_order;
    int m_shutdowns = 0;
};

} // namespace fugue
