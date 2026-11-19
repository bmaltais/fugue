#pragma once

// The cross-process run queue of spec 9.8: a folder with one small JSON file per
// waiting or active run. The file name sorts into queue order:
//   <class>-<enqueue time in ms, 14 digits>-<run id>
// where class is 0 for the run that is active, 3 for a run queued with --first and
// 5 for the others. So "--first" goes ahead of the waiting runs but never ahead of the
// run that is already going, and equal classes keep their arrival order.
//
// A process starts its run only when its entry sorts first and it holds the advisory
// lock (engine.lock, see Lock). An entry whose process is gone is stale and discarded.

#include <QJsonObject>
#include <QList>
#include <QString>

namespace fugue::cli {

struct QueueEntry {
    QString file; // the file name inside the queue folder
    QString run;
    qint64 pid = 0; // the worker's process id; 0 until it is known
    QString workflow;
    bool active = false;
};

class Queue {
public:
    explicit Queue(QString dir) : m_dir(std::move(dir)) {}

    // $XDG_STATE_HOME/fugue/queue (~/.local/state/fugue/queue), and the lock beside it.
    static QString defaultDir();
    static QString defaultLockPath();

    // Adds an entry for `run`. `first` sorts it ahead of the waiting runs.
    bool enqueue(const QString &run, const QString &workflow, bool first) const;
    // In queue order.
    QList<QueueEntry> entries() const;
    // The entry of a run, if it has one.
    bool find(const QString &run, QueueEntry *out = nullptr) const;
    // Records the worker's process id in the run's entry.
    bool setPid(const QString &run, qint64 pid) const;
    // Moves the run's entry to the active class. False when the entry is gone.
    bool activate(const QString &run) const;
    bool remove(const QString &run) const;
    // Removes the entries whose process is gone; returns the runs they were for.
    QStringList discardStale() const;

private:
    bool write(const QString &file, const QJsonObject &content) const;
    QString m_dir;
};

// The advisory lock of 9.8. Held for the life of the object; the system releases
// it if the process dies.
class Lock {
public:
    explicit Lock(const QString &path);
    ~Lock();
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
    // Tries once, without waiting.
    bool tryAcquire();

private:
    int m_fd = -1;
    bool m_held = false;
};

bool processExists(qint64 pid);

} // namespace fugue::cli
