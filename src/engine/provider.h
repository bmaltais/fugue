#pragma once

// The kernel provider interface (spec 8.11). It has exactly the operations of the provider
// protocol (8.8): describe, submit, status (polled, or streamed by statusChanged), cancel,
// result and shutdown, with the same data in and out. The engine only ever talks to this
// interface; the omarchy adapter and the fake provider implement it.
//
// Every operation is asynchronous: its reply arrives later from the event loop, never inside
// the call, so the scheduler can drive many providers from one thread.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

#include "engine/error.h"
#include "engine/result.h"

namespace fugue {

template <typename T> using Reply = std::function<void(const Result<T> &)>;

// The value of an operation that returns nothing (shutdown).
struct Nothing {};

// Whether a model can take a job now (8.8). A module's own word for it is read with
// parseModelStatus: anything that is not ready or loading is unavailable.
enum class ModelStatus { Ready, Loading, Unavailable };
// "ready", "loading", "unavailable". Not a free toString(): QtTest takes one found by
// argument lookup for its own char *toString() and fails to compile QCOMPARE.
QString modelStatusName(ModelStatus status);
ModelStatus parseModelStatus(const QString &text);

// One model a provider can serve for a capability. Status, properties, limits and defaults are
// per model, not per capability as 8.8's table reads, because one omarchy instance can run
// several image servers at once with different defaults and edit support.
struct OfferedModel {
    // A resource handle (2.5); served-by-name models have no hash. Empty for a provider that
    // takes its model files in the job (the helper's stage capabilities, 8.10): it is chosen for
    // "any ready model", and a model picker does not list it.
    QJsonObject handle;
    QString title;          // the name people know it by ("FLUX.2 Klein 4B"); display only, not in the handle
    ModelStatus status = ModelStatus::Unavailable;
    QString reason;         // why it is not ready, one sentence
    QString hint;           // how to make it ready, for the 8.5 error
    QJsonObject properties; // for routing (8.1): e.g. {"edit": true, "input": ["text"]}
    QJsonObject limits;     // min_width, max_width, min_height, max_height, ...
    QJsonObject defaults;   // steps, guidance, width, height
};

struct CapabilityOffer {
    QString capability; // an open id such as image.generate (8.1)
    QList<OfferedModel> models;

    // Ready when any model is, else loading when any is, else unavailable.
    ModelStatus status() const;
};

// The reply of describe (8.8).
struct Description {
    int protocol = 1;
    QString engine; // stable id of the inference engine, used in cache keys (8.11)
    QList<CapabilityOffer> capabilities;
    // False for a provider on another machine (a paired helper, 8.10): it cannot read this
    // machine's files, so media go to it as data and models as handles it resolves itself.
    bool sharesFilesystem = true;
    // The memory the provider's machine has free, when it says (8.10).
    std::optional<qint64> freeMemoryBytes;

    const CapabilityOffer *offer(const QString &capability) const;
};

// The request of submit (8.8), plus where to put the results.
struct JobRequest {
    QString capability;
    QJsonObject model;  // a resource handle; empty means any ready model (8.4)
    QJsonObject params; // the standard parameters of the capability (8.1)
    QJsonObject inputs; // media, as {"path": …} or an array of them
    // Kernel-only: the directory the provider writes results to the moment the job finishes
    // (8.6). The caller creates it; result() returns paths inside it.
    QString outputDir;
};

// The reply of status (8.8).
struct JobStatus {
    QString state;      // queued, running, done, error or cancelled
    double progress = 0; // 0 to 1, of the current pass when the engine reports passes
    QString stage;
    QByteArray preview; // the latest preview frame (PNG bytes), when the provider sends one
    Error error;        // set when state is error

    bool isTerminal() const;
};

// The reply of result (8.8).
struct JobResult {
    // Values and file references, e.g. {"image": {"path": "/abs/image.png", "format": "png"}}.
    QJsonObject outputs;
    // The values actually used, e.g. seed, width, height, steps, model, engine (8.1).
    QJsonObject used;
};

class Provider : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~Provider() override;

    virtual void describe(Reply<Description> reply) = 0;
    // The reply's value is the job id, unique within this provider.
    virtual void submit(const JobRequest &request, Reply<QString> reply) = 0;
    virtual void status(const QString &job, Reply<JobStatus> reply) = 0;
    // Cooperative: the reply is the state right after asking. A running job may stay running
    // until the engine reaches a checkpoint; wait for a terminal status (9.6).
    virtual void cancel(const QString &job, Reply<QString> reply) = 0;
    // Only for a job whose state is done; its files are already in the request's outputDir.
    virtual void result(const QString &job, Reply<JobResult> reply) = 0;
    // Only for providers Fugue started; the default has nothing to stop.
    virtual void shutdown(Reply<Nothing> reply);

signals:
    // The status stream: emitted on every change of a submitted job's status.
    void statusChanged(const QString &job, const fugue::JobStatus &status);

protected:
    // Delivers a reply from the event loop, after the current call has returned.
    template <typename T> void later(Reply<T> reply, Result<T> outcome) {
        deliverLater([reply = std::move(reply), outcome = std::move(outcome)] { reply(outcome); });
    }

private:
    void deliverLater(std::function<void()> deliver);
};

} // namespace fugue
