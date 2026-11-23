#include "fake_provider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace fugue {

namespace {

Error unknownJob(const QString &job) {
    return makeError(QStringLiteral("internal"), QStringLiteral("No job %1.").arg(job));
}

} // namespace

FakeProvider::FakeProvider(Description description, QObject *parent)
    : Provider(parent), m_description(std::move(description)) {}

void FakeProvider::setDescription(const Description &description) { m_description = description; }

void FakeProvider::setDescribeErrors(const Errors &errors) { m_describeErrors = errors; }

void FakeProvider::describe(Reply<Description> reply) {
    later(std::move(reply), m_describeErrors.isEmpty() ? Result<Description>(m_description)
                                                       : Result<Description>(m_describeErrors));
}

void FakeProvider::submit(const JobRequest &request, Reply<QString> reply) {
    const QString id = QStringLiteral("fake-%1").arg(m_order.size() + 1);
    Job job;
    job.request = request;
    job.status.state = QStringLiteral("queued");
    m_jobs.insert(id, job);
    m_order.append(id);
    later(std::move(reply), Result<QString>(id));
}

void FakeProvider::status(const QString &job, Reply<JobStatus> reply) {
    if (!m_jobs.contains(job))
        return later(std::move(reply), Result<JobStatus>(unknownJob(job)));
    later(std::move(reply), Result<JobStatus>(m_jobs.value(job).status));
}

void FakeProvider::cancel(const QString &job, Reply<QString> reply) {
    if (!m_jobs.contains(job))
        return later(std::move(reply), Result<QString>(unknownJob(job)));
    if (!m_jobs.value(job).status.isTerminal()) {
        JobStatus cancelled;
        cancelled.state = QStringLiteral("cancelled");
        change(job, cancelled);
    }
    later(std::move(reply), Result<QString>(m_jobs.value(job).status.state));
}

void FakeProvider::result(const QString &job, Reply<JobResult> reply) {
    if (!m_jobs.contains(job))
        return later(std::move(reply), Result<JobResult>(unknownJob(job)));
    const Job &found = m_jobs[job];
    if (found.status.state != QLatin1String("done"))
        return later(std::move(reply),
                     Result<JobResult>(makeError(QStringLiteral("internal"),
                                                           QStringLiteral("Job %1 is %2, not done.")
                                                               .arg(job, found.status.state))));
    later(std::move(reply), Result<JobResult>(found.result));
}

void FakeProvider::shutdown(Reply<Nothing> reply) {
    ++m_shutdowns;
    later(std::move(reply), Result<Nothing>(Nothing{}));
}

JobRequest FakeProvider::submitted(const QString &job) const { return m_jobs.value(job).request; }

QStringList FakeProvider::jobs() const { return m_order; }

void FakeProvider::progress(const QString &job, double value, const QString &stage) {
    JobStatus running;
    running.state = QStringLiteral("running");
    running.progress = value;
    running.stage = stage;
    change(job, running);
}

void FakeProvider::preview(const QString &job, const QByteArray &frame) {
    JobStatus running;
    running.state = QStringLiteral("running");
    running.preview = frame;
    change(job, running);
}

void FakeProvider::finish(const QString &job, const QMap<QString, QByteArray> &files,
                          const QJsonObject &used) {
    if (!m_jobs.contains(job))
        return;
    const QDir dir(m_jobs[job].request.outputDir);
    QJsonObject outputs;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QString path = dir.filePath(it.key());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(it.value()) != it.value().size())
            return fail(job, makeError(QStringLiteral("internal"),
                                           QStringLiteral("Could not write %1.").arg(path)));
        const QFileInfo info(path);
        outputs.insert(info.completeBaseName(),
                       QJsonObject{{"path", info.absoluteFilePath()}, {"format", info.suffix()}});
    }
    m_jobs[job].result = JobResult{outputs, used};
    JobStatus done;
    done.state = QStringLiteral("done");
    done.progress = 1;
    change(job, done);
}

void FakeProvider::fail(const QString &job, const Error &error) {
    JobStatus failed;
    failed.state = QStringLiteral("error");
    failed.error = error;
    change(job, failed);
}

void FakeProvider::change(const QString &job, const JobStatus &status) {
    if (!m_jobs.contains(job))
        return;
    m_jobs[job].status = status;
    emit statusChanged(job, status);
}

} // namespace fugue
