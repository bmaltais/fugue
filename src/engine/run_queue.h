#pragma once

// The run queue of one process (spec 9.2): one run is active at a time, and
// further runs wait first in, first out. A waiting run can be moved to the
// front ("run first") or removed, and the active one cancelled, by run id.
//
// This is the queue inside a process: the canvas uses it for its own runs.
// Sharing the GPU between processes (the queue folder and lock of 9.8) is
// built on top by the CLI; a run worker there calls runGraph() directly.
//
// Like Run, it lives on one thread with an event loop.

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

#include "engine/scheduler.h"

namespace fugue {

class RunQueue : public QObject {
    Q_OBJECT

public:
    explicit RunQueue(const RunEnvironment &environment, QObject *parent = nullptr);
    ~RunQueue() override;

    // Queues a run and returns its id (the run folder's). run.json gets the
    // status `queued` and events.jsonl a run.queued event. The run starts at
    // once when nothing is active. With `first` it goes ahead of the runs
    // already waiting.
    QString enqueue(const CompiledGraph &graph, const RunFolder &folder, const RunOptions &options = {},
                    bool first = false);

    // "Run first": moves a waiting run ahead of the others. False when the
    // id is not waiting.
    bool moveToFront(const QString &id);

    // Removes a waiting run (its run.json and events end with status
    // `cancelled`) or cancels the active one. False when the id is neither.
    bool cancel(const QString &id);

    QString active() const;       // the id of the run going now, or empty
    QStringList waiting() const;  // in the order they will run

signals:
    void runStarted(const QString &id);
    // Every event of every run, as written to its events.jsonl.
    void runEvent(const QString &id, const QJsonObject &event);
    void runFinished(const QString &id, const fugue::RunOutcome &outcome);

private:
    struct Waiting {
        CompiledGraph graph;
        RunFolder folder;
        RunOptions options;
    };

    void startNext();
    void record(const RunFolder &folder, const QJsonObject &event);

    RunEnvironment m_environment;
    QList<Waiting> m_waiting;
    std::unique_ptr<Run> m_active;
};

} // namespace fugue
