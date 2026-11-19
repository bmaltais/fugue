#pragma once

// The cross-process run queue of spec 9.8: a folder with one small JSON file per
// waiting or active run. The file name sorts into queue order:
//   <class>-<enqueue time in ms, 14 digits>-<run id>
// where class is 0 for the run that is active, 3 for a run queued with --first and
// 5 for the others. So "--first" goes ahead of the waiting runs but never ahead of the
// run that is already going, and equal classes keep their arrival order.
//
// A process starts its run only when its entry sorts first and it holds the advisory
// lock (engine.lock, see Lock). An entry whose process is gone is dead and discarded
// (discardDead).
//
// Every change to the folder is made under a short lock of its own (the hidden file .lock
// in it), so two processes never interleave a find with a rename, rewrite or removal.
// Readers need no lock: entries appear, move and go atomically.

#include <QJsonObject>
#include <QList>
#include <QString>

namespace fugue::cli {

class Lock;

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
    // Records the worker's process id in the run's entry. An entry that is gone (a
    // cancelled run) stays gone.
    bool setPid(const QString &run, qint64 pid) const;
    // Moves the run's entry to the active class. False when the entry is gone.
    bool activate(const QString &run) const;
    // Removes the run's entry, whatever its class: for the process that owns it.
    bool remove(const QString &run) const;
    // Removes exactly the entry that was seen, by its file. False when that file is gone:
    // the run was activated (its file renamed) or removed since `seen` was read. Whoever
    // gets true is the one process that took the entry out of the queue.
    bool remove(const QueueEntry &seen) const;
    // Removes the entries whose process is gone and returns the runs they were for; the
    // caller ends those runs. A waiting entry is dead when its pid is not that run's worker
    // (isRunWorker), or when it never got a pid within 30 seconds. An active entry is dead
    // when `engineLock` can be taken: its owner held that lock for as long as it lived. The
    // lock is left as it was found (still held if the caller held it). Not for a process
    // that is itself running a run.
    QStringList discardDead(Lock &engineLock) const;

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
    // Tries once, without waiting. True when it is (already) held.
    bool tryAcquire();
    bool held() const { return m_held; }
    void release();

private:
    int m_fd = -1;
    bool m_held = false;
};

// Whether process `pid` is the worker of `run`: its command line holds `__worker <run>`
// (read from /proc/<pid>/cmdline). A process id alone proves nothing, because ids are
// reused; this cannot be fooled by another process that got the id.
bool isRunWorker(qint64 pid, const QString &run);

} // namespace fugue::cli
