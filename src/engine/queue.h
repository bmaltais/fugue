#pragma once

// The run queue of spec 9.8, shared by the window and every run worker because they
// share the GPU: a folder with one small JSON file per
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

namespace fugue {

class Lock;

struct QueueEntry {
    QString file; // the file name inside the queue folder
    QString run;
    qint64 pid = 0; // the worker's process id; 0 until it is known
    QString workflow;
    // When it was queued, in CLOCK_BOOTTIME milliseconds ("queued_boottime_ms"); 0 in an
    // entry written before the field existed.
    qint64 queuedBootMs = 0;
    bool active = false;
    // The run belongs to a process that runs it itself (the window, 9.8), not to a
    // `fugue __worker`: "owner": "window" with "process_start", the start time of that
    // process (field 22 of /proc/<pid>/stat), which tells it from a later process that got
    // the same pid.
    bool inProcess = false;
    qint64 processStart = 0;
};

class Queue {
public:
    explicit Queue(QString dir) : m_dir(std::move(dir)) {}

    // $XDG_STATE_HOME/fugue/queue (~/.local/state/fugue/queue), and the lock beside it.
    static QString defaultDir();
    static QString defaultLockPath();
    // The lock a run takes besides the engine lock, across roots (spec 1.4.2): one file in the real
    // runtime folder, the same for every project, so two roots never run on the GPU at once. Empty for
    // any engine lock but the default one (a test's own folder has no business with the machine's).
    static QString crossRootLockFor(const QString &engineLockPath);

    // Adds an entry for `run`. `first` sorts it ahead of the waiting runs.
    bool enqueue(const QString &run, const QString &workflow, bool first) const;
    // The same for a run this process runs itself (the window): the entry records this
    // process and is live for as long as it lives (discardDead).
    bool enqueueForThisProcess(const QString &run, const QString &workflow, bool first) const;
    // In queue order.
    QList<QueueEntry> entries() const;
    // The entry of a run, if it has one.
    bool find(const QString &run, QueueEntry *out = nullptr) const;
    // Records the worker's process id in the run's entry; the worker calls it for itself.
    // An entry that is gone (a cancelled run) stays gone.
    bool setPid(const QString &run, qint64 pid) const;
    // Moves the run's entry to the active class. False when the entry is gone.
    bool activate(const QString &run) const;
    // Moves a waiting run's entry ahead of every other waiting one (the queue's "Queue Front"):
    // class 3 and a time before the earliest of the entries already in that class. True when it
    // is first among the waiting runs now; false for the active run and for a run with no entry.
    bool moveToFront(const QString &run) const;
    // Removes the run's entry, whatever its class: for the process that owns it.
    bool remove(const QString &run) const;
    // Removes exactly the entry that was seen, by its file. False when that file is gone:
    // the run was activated (its file renamed) or removed since `seen` was read. Whoever
    // gets true is the one process that took the entry out of the queue.
    bool remove(const QueueEntry &seen) const;
    // Removes the entries whose process is gone and returns the runs they were for; the
    // caller ends those runs. A waiting entry is dead when its pid is not that run's worker
    // (isRunWorker), or when it never got a pid within 30 seconds of being queued (on the
    // boot clock, so wall-clock jumps do not count; an entry without that time is aged by
    // its file's modification time, and one queued before a reboot is dead). An active
    // entry is dead when `engineLock` can be taken: its owner held that lock for as long as
    // it lived. The lock is left as it was found (still held if the caller held it). A
    // waiting entry of a process that runs its own runs (enqueueForThisProcess) is dead when
    // no process with its pid and start time is left; the caller's own entries are never
    // dead, so a process that holds the lock for its own run may call this too.
    QStringList discardDead(Lock &engineLock) const;

private:
    bool write(const QString &file, const QJsonObject &content) const;
    QString m_dir;
};

// The advisory lock of 9.8. Held for the life of the object; the system releases
// it if the process dies.
class Lock {
public:
    // `crossRoot`, when not empty, is a second file locked and released together with the first, in
    // that order; a try that gets the first but not the second lets the first go again.
    explicit Lock(const QString &path, const QString &crossRoot = {});
    ~Lock();
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
    // Tries once, without waiting. True when it is (already) held.
    bool tryAcquire();
    bool held() const { return m_held; }
    void release();

    // Whether a run of another root has been waiting for the cross-root lock `crossRoot` lately (a failed try
    // there leaves a time stamp in `<crossRoot>.wait`, at most every half second, so a waiter that is gone
    // stops counting after `freshMs`). The holder asks it to give the GPU's memory back for the one that waits.
    static bool otherRootWaiting(const QString &crossRoot, int freshMs = 3000);

    // The process that holds the lock file `path` (an flock, read from /proc/locks and confirmed through the
    // holder's open files), other than `exceptPid`; 0 when nobody does or the kernel does not say. Only for telling
    // a person who is in the way; never for deciding anything.
    static qint64 holderPid(const QString &path, qint64 exceptPid = 0);

private:
    int m_fd = -1;
    int m_crossFd = -1;
    QString m_crossPath;
    qint64 m_lastWaitMark = 0;
    bool m_held = false;
};

// Whether process `pid` is the worker of `run`: its command line holds `__worker <run>`
// (read from /proc/<pid>/cmdline). A process id alone proves nothing, because ids are
// reused; this cannot be fooled by another process that got the id. A process has no
// command line for a moment while it starts, so a pid is only checked once the worker has
// recorded it itself (Queue::setPid from the worker), never one a parent got from spawning.
bool isRunWorker(qint64 pid, const QString &run);

// The start time of process `pid` in clock ticks since boot (field 22 of /proc/<pid>/stat),
// or 0 when there is no such process. A pid and this together name one process.
qint64 processStartTime(qint64 pid);

// Sends `signal` to process `pid` only if it is the worker of `run`, and returns whether
// it was sent. The process is pinned by a pidfd before its command line is checked, and
// the signal goes through that pidfd, so it reaches the process that was checked or
// nobody: never one that got the id after the worker died. Every signal to a run's worker
// goes through here, not kill().
bool signalRunWorker(qint64 pid, const QString &run, int signal);

} // namespace fugue
