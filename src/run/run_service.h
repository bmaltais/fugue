#pragma once

// The window's runs (spec 9.8: "The window runs its own runs"), off the UI thread.
//
// RunService is the window's end of the engine. It owns one thread, the run thread, and
// everything a run touches lives there: its own Registry (node types, filled again after a
// reloadNodeTypes(), and the provider instances read again at each run's turn, as a
// `fugue __worker` does; an instance that has not changed keeps its provider, so the helper
// and the models it holds stay between runs, spec 9.3), the kernels, the
// result cache and file-hash memo (cli::Process), the Run objects and the engine lock. The UI
// thread only hands over a copy of the document and gets events back, so nothing it does
// waits on compiling, hashing, moving outputs into the store, a provider or the queue.
//
// A run started here goes through the same steps as `fugue run`: compile (with "run to
// here" as CompileOptions::target), a run folder (compiled.json, workflow.json, run.json,
// run.queued), an entry in the queue the window shares with every CLI worker
// (Queue::enqueueForThisProcess: live while the window lives), its turn (first in the queue
// and the engine lock, looked at every 50 ms on the run thread), then the scheduler's Run.
// A run that does not compile never gets a folder, like `fugue run`: it ends at once with a
// run.done whose errors are the compiler's.
//
// Video and audio results get {"label": "Video · 0:04", "width"?, "height"?} in place of a
// thumbnail, and a result with a file (Save Image's) also "file": its file name.
//
// Events come back as event(run, {...}) on the UI thread: the scheduler's (9.4), plus the
// run.queued and run.done of runs that never reached the scheduler. node.done, node.cached
// and node.preview also carry "thumbnails": {<output name>: {"url", "width", "height"}} for
// each image they hold (`shown.value` for a sink): a copy at most 512 px wide, made once
// per image on the run thread and kept under the thumbnail folder by content hash, so the
// canvas loads a small file (file:// URL) and never decodes a full-size result.
//
// check() is `fugue validate`'s view of the nodes (10.3) for the canvas: cached, stale,
// frozen or skipped, by path id, made on the run thread while no run is active (checkGraph
// waits on providers with an event loop of its own, which must not run inside a run), the
// latest request only. A check gives way to runs: one asked for, or a waiting one whose turn
// in the queue comes, ends it at once (a paired host's describe can take its whole transfer
// timeout), and it is made again once no run is active.
//
// Cancel (9.6): a waiting run leaves the queue and ends as cancelled; an active one is
// cancelled cooperatively. `fugue runs cancel` reaches an active window run through the run
// folder (RunFolder::requestCancel), which the run thread looks at while it runs.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <optional>

#include "engine/document.h"
#include "engine/error.h"
#include "engine/file_roots.h"
#include "engine/node_kernel.h"
#include "engine/registry.h"

class QThread;

namespace fugue {

class RunThread;

// Where a window's runs keep their files and how its registry is filled. The defaults are
// the user's folders (spec 1.4); tests point every path into a temporary folder.
struct RunSetup {
    std::function<void(Registry &)> addNodeTypes;
    std::function<void(Kernels &)> addKernels;
    // Puts the provider instances into a registry, on the run thread (providers are QObjects
    // that answer through its event loop). Default: cli::registerProviders (providers.json).
    std::function<Errors(Registry &)> registerProviders;
    // The canvas last used in a Canvas view, from any thread (canvas/active_canvas.h); a Canvas Region with no
    // canvas chosen reads it (#246). None: the newest canvas of the root.
    std::function<QString()> activeCanvas;
    QString runsRoot;     // default RunFolder::defaultRoot()
    QString queueDir;     // default Queue::defaultDir()
    QString lockPath;     // default Queue::defaultLockPath()
    QString cacheRoot;    // default ResultCache::defaultRoot()
    QString hashesPath;   // default FileHashes::defaultPath()
    QString trustPath;    // default TrustStore::defaultPath(): the packs the user allowed (6.7)
    QString consentPath;  // default RemoteConsent::defaultPath(): the off-machine services the user allowed (8.2, #404)
    QString thumbnailDir; // default <cache>/thumbnails
    // The file roots of 13.6 the runs are held to; none: not limited (spec Appendix A, #19).
    std::optional<FileRoots> roots;
    // Run folders and the result store are collected (10.5) at most once an hour, as the
    // CLI worker does; off in tests, whose folders are temporary.
    bool collectGarbage = true;
};

struct RunRequest {
    Document document;
    QString workflow; // the file it came from, absolute, or empty for an unsaved one
    QString target;   // "run to here": the path id to stop at, or empty
    QString entered;  // the group instance the canvas shows, as a path id: its sinks work (5.5)
    bool first = false; // ahead of the waiting runs ("run first")
    // Path ids whose pictures this run does not make (#263): the canvas shows none for them.
    QSet<QString> noPictures;
};

class RunService : public QObject {
    Q_OBJECT

public:
    explicit RunService(RunSetup setup, QObject *parent = nullptr);
    // Cancels what is running, waits for it to end (at most a few seconds, then ends its
    // folder as cancelled), and stops the run thread.
    ~RunService() override;

    // Starts a run and returns its id at once; everything else happens on the run thread.
    QString start(const RunRequest &request);
    void cancel(const QString &run);
    // The person's answer to remoteConsentNeeded for a run: yes keeps it (per instance) and the run goes on, no cancels the run.
    void answerRemote(const QString &run, bool allow);
    // The state of every node, answered by checked() with the same ticket. A newer request
    // replaces one not made yet.
    void check(const RunRequest &request, int ticket);

    // A thumbnail of the image file at `path` (a Load Image's input, before any run), made on
    // the run thread like a result's (cached by path, time and size) and answered by
    // filePreviewed() with the same token: {"url", "width", "height"}, or {"missing": true}
    // for a file that is gone, unreadable, not an image or outside the runs' file roots.
    void previewFile(const QString &token, const QString &path);

    // The results of a finished run again, answered by event() with the run's own id: its node
    // and run.done events from the run folder (see RunThread::replay).
    void replay(const QString &run);
    // The folder the run folders are in (RunSetup::runsRoot, else the user's): what History and the App view's takes read.
    QString runsRoot() const { return m_runsRoot; }

    // Calls `call` on the run thread and waits for it (tests: a FakeProvider lives there).
    void onRunThread(const std::function<void()> &call);

    // The packs changed (#70): the run thread fills its registries again (RunSetup::addNodeTypes)
    // before it next uses each, never under a run or a check in flight. The next compile (a run
    // started, a check) takes the new types, and the next run to start runs them, so an updated
    // command node runs its new files. A run already waiting keeps what it was compiled to.
    // Takes effect when this returns (the count is atomic, not queued behind a start under way, #223).
    void reloadNodeTypes();

signals:
    void event(const QString &run, const QJsonObject &event);
    // nodes: {<path id>: {"state", "reason"?, "runsOn"? ("<instance> (<host>)" for a node routed off this machine)}}; problems: the compiler's and checkGraph's
    // errors and warnings;
    // checkErrors: only what the nodes' kernels' check() found (#600), the errors that show on their nodes.
    void checked(int ticket, const QJsonObject &nodes, const QJsonArray &problems, const QJsonArray &checkErrors);
    void filePreviewed(const QString &token, const QString &path, const QJsonObject &thumbnail);
    // A run would send data off this machine to services the person has not allowed (8.2, #404): `services` is
    // [{"instance", "host", "sends": [..], "text"}]. The run waits (no folder, not queued) for answerRemote.
    void remoteConsentNeeded(const QString &run, const QJsonArray &services);

private:
    QString m_runsRoot;
    QThread *m_thread;
    RunThread *m_worker;
};

} // namespace fugue
