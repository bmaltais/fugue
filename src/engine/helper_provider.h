#pragma once

// The fugue-helper provider (spec 8.7, 8.10): Fugue's own generation helper (helper/, on
// stable-diffusion.cpp with the stage patch), spoken to over the stdio transport of 8.8.
//
// - Started on demand: on the first submit, not by describe (a model picker describes every
//   provider each time it opens, and that must not start a GPU process).
// - Kept warm while it has jobs and for `idle_timeout_s` after the last one, then sent
//   `shutdown` (and killed if it does not exit within the grace period) to give its memory back.
// - One job at a time: the helper runs the jobs it is sent in order.
// - A helper that dies fails the jobs it held (node_failed, with the last lines of its log) and
//   is started again on the next submit. Out of memory is the job's `out_of_memory`.
// - Cancel is cooperative (the helper stops at its next step); a running job that is not over
//   within the grace period is ended by killing the helper.
//
// The stage capabilities take their models as files: params.context names them by path, as
// stable-diffusion.cpp's context options do, so a capability's one offered model has an empty
// handle (provider.h, OfferedModel). Results are written into the job's outputDir by the helper
// itself (it shares the filesystem).

#include <QHash>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <functional>

#include "engine/providers.h"

namespace fugue {

// The capabilities the helper serves (8.1).
QStringList helperCapabilities();

// Where the helper is, in the order looked at: FUGUE_HELPER when set; else `lib/fugue` beside the
// app's own folder (`<prefix>/bin/fugue` -> `<prefix>/lib/fugue`), then a source checkout's
// `build-helper-<backend>/` beside the app's build folder. In each folder the CUDA build
// (`fugue-helper-cuda`) wins on an NVIDIA GPU, then the Vulkan one, then a CPU one (a developer
// build). Empty when there is none.
QString helperProgram(bool nvidia);
QString helperProgram(const QStringList &folders, bool nvidia);

// Whether this machine has an NVIDIA GPU (its kernel driver is loaded).
bool hasNvidiaGpu();

class HelperProvider : public Provider {
    Q_OBJECT

public:
    // `program` is the helper to start; empty when none is installed, which makes every
    // operation fail with provider_unreachable. The instance's `idle_timeout_s` setting (seconds,
    // a number or its text; default 600; 0 never stops it for idling) is how long it stays warm.
    HelperProvider(const ProviderInstance &instance, QString program, int graceMs = 5000,
                   QObject *parent = nullptr);
    ~HelperProvider() override;

    void describe(Reply<Description> reply) override;
    void submit(const JobRequest &request, Reply<QString> reply) override;
    void status(const QString &job, Reply<JobStatus> reply) override;
    void cancel(const QString &job, Reply<QString> reply) override;
    void result(const QString &job, Reply<JobResult> reply) override;
    // Stops the helper now: `shutdown`, then killed after the grace period; returns once it is
    // gone, so its memory is back. Jobs it held fail.
    void shutdown(Reply<Nothing> reply) override;

    // The helper's process id while it runs, else 0.
    qint64 pid() const;

private:
    // A reply of the helper's, or why there is none (its error, or it stopped).
    using Answer = std::function<void(const Result<QJsonObject> &)>;

    struct Job {
        JobStatus status;
        JobResult result;
        int generation = 0; // the helper process it ran in
        QString helperJob;  // its id in that process
        bool cancelled = false; // asked to stop: a helper that dies meanwhile cancels it
    };

    // Starts a helper; why not, when it failed at once.
    QString start();
    void send(QJsonObject message, Answer answer);
    void readOutput(QProcess *process);
    // One protocol line: a reply, or a status the helper sent unasked.
    void line(const QJsonObject &message);
    void helperStatus(const QString &helperJob, const QJsonObject &message);
    // A job's new status, emitted.
    void change(const QString &job, const JobStatus &status);
    // The helper process ended.
    void finished();
    // Fails the replies waited for with `why`, and the jobs the helper held.
    void abandon(const Error &why);
    // Lets go of the helper: asked to shut down, killed after the grace period. The process
    // while it is still running, else null.
    QProcess *stop();
    void updateIdle();
    Error unreachable(const QString &why = {}) const;
    static Error stopped(const QString &why);
    static QList<QByteArray> takeLines(QByteArray &buffer);
    QString lastLog() const;

    ProviderInstance m_instance;
    QString m_program;
    int m_graceMs;
    int m_idleMs;

    QProcess *m_process = nullptr;
    int m_generation = 0;
    int m_nextId = 0;
    QByteArray m_out;
    QByteArray m_err;
    QStringList m_log; // the helper's last stderr lines
    QString m_startError; // why the last helper could not be started
    QHash<QString, Answer> m_waiting; // request id -> what to do with the reply
    QHash<QString, Job> m_jobs;       // by our job id
    QHash<QString, QString> m_byHelperJob; // the running helper's job id -> ours, while not over
    QTimer m_idle;
};

} // namespace fugue
