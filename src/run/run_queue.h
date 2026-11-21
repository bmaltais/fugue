#pragma once

// The run queue as a window lists it (spec 9.8, 12.4): the runs going and waiting in the queue the
// window shares with every `fugue run` worker, and a short history of the runs that ended. It
// reads the queue folder and the run folders and writes through the same operations the CLI
// uses, so a run started from a terminal shows here and a run started here shows in
// `fugue runs list`:
//   - cancel: a run of this window goes through RunService; any other the way `fugue runs
//     cancel` does (cli::cancelRun: take a waiting entry out, ask a window, signal a worker);
//   - move to front: Queue::moveToFront on a waiting entry.
// It knows nothing of the canvas or of what a run started from; QML reads it as App.runQueue.
//
// What it holds is kept current without polling at idle: a watch on the queue folder, and the
// run events of this window. Only while `watching` (the queue panel is open) does it also read
// the going run's events each second for its node and progress, and list the history.
//
// A row of `entries` (queue order, the active run first) is
//   {run, name, workflow, status: "running"|"waiting", position (0 running, 1.. waiting),
//    source: "window"|"cli", mine (a run of this window), since (ms since the epoch: when it
//    started, or was queued), node (path id of the node going, or ""), progress (0..1, or -1)}
// and a row of `history` (newest first)
//   {run, name, workflow, status: "ok"|"failed"|"cancelled", seconds (or -1), ended (ms)}.

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>

namespace fugue {

class RunService;

struct RunQueuePaths {
    QString runsRoot; // default RunFolder::defaultRoot()
    QString queueDir; // default Queue::defaultDir()
    QString lockPath; // default Queue::defaultLockPath()
};

class RunQueue : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(QVariantList history READ history NOTIFY changed)
    // Runs going and waiting, of any process.
    Q_PROPERTY(int count READ count NOTIFY changed)
    // True while the panel that shows the rows is open: progress and the history are kept current.
    Q_PROPERTY(bool watching READ watching WRITE setWatching NOTIFY watchingChanged)

public:
    static constexpr int historyLimit = 8;

    // `service` is the window's own (cancelling its runs); `openUrl` opens a run's folder.
    explicit RunQueue(RunQueuePaths paths, RunService *service = nullptr, QObject *parent = nullptr);
    void setOpenUrl(std::function<void(const QUrl &)> openUrl) { m_openUrl = std::move(openUrl); }

    QVariantList entries() const { return m_entries; }
    QVariantList history() const { return m_history; }
    int count() const { return int(m_entries.size()); }
    bool watching() const { return m_watching; }
    void setWatching(bool watching);

    // Cancels a run, wherever it is: the window's or another process's, waiting or going.
    Q_INVOKABLE void cancel(const QString &run);
    // A waiting run goes ahead of the other waiting runs (not ahead of the one going).
    Q_INVOKABLE bool moveToFront(const QString &run);
    // The run's folder, in the file manager.
    Q_INVOKABLE void openFolder(const QString &run);
    // The workflow file a run started from (empty for an unsaved one), the same as in its rows.
    Q_INVOKABLE QString workflowOf(const QString &run) const;
    // Reads the queue and the folders now (tests, and the one place that wants it at once).
    Q_INVOKABLE void refresh();

signals:
    void changed();
    void watchingChanged();

private:
    void schedule() { m_later.start(); }

    RunQueuePaths m_paths;
    RunService *m_service;
    std::function<void(const QUrl &)> m_openUrl;
    QFileSystemWatcher m_watcher;
    QTimer m_later; // coalesces a burst of changes
    QTimer m_tick;  // one second while watching: the going run's progress
    QVariantList m_entries;
    QVariantList m_history;
    bool m_watching = false;
};

} // namespace fugue
