#pragma once

// The run folder (spec 9.7): ~/.cache/fugue/runs/<run-id>/ holding
//   run.json       the run's state (its contents belong to the scheduler)
//   compiled.json  the compiled graph that ran
//   workflow.json  the workflow document it was compiled from, when there was one
//   roots.json     the folders its nodes may read and write (file_roots.h), when limited
//   events.jsonl   every event, one compact JSON object per line
//   nodes/<path id>/  each node's working folder; a path id's groups become
//                  nested folders, so g7/first is nodes/g7/first/.
//
// Files are written so that another process (`fugue runs status|events`) can
// read them at any time: run.json and compiled.json are replaced atomically,
// and each event is appended as one whole line.

#include <QJsonObject>
#include <QList>

#include <optional>
#include <QString>

#include "engine/clock.h"
#include "engine/file_roots.h"
#include "engine/result.h"

namespace fugue {

// A run's status, as run.json and run.done write it (9.4, 9.7).
enum class RunStatus { Unknown, Queued, Running, Ok, Failed, Cancelled };
// "unknown", "queued", "running", "ok", "failed", "cancelled". (Not a free toString():
// QtTest would take it for its own.)
QString runStatusName(RunStatus status);
RunStatus parseRunStatus(const QString &text); // Unknown for anything else
// Ok, failed or cancelled: the run is over.
bool isTerminal(RunStatus status);

// A new run id: the UTC start time to the millisecond and four random
// characters, e.g. "20261119-153012-123-k3f9". Opaque, of [a-z0-9-], and
// sortable by start time; ids made in one process sort in the order they were
// made. Uniqueness across processes comes from RunFolder::create.
QString newRunId();

class RunFolder {
public:
    // $XDG_CACHE_HOME/fugue/runs (~/.cache/fugue/runs).
    static QString defaultRoot();

    // Makes a new run folder under `root`, with a new id unless one is given.
    // A given id that is taken or not of [a-z0-9-] fails.
    static Result<RunFolder> create(const QString &root, const QString &id = {});
    // An existing run folder; unknown_run when there is none.
    static Result<RunFolder> open(const QString &root, const QString &id);

    QString id() const { return m_id; }
    QString path() const { return m_path; }
    QString runJsonPath() const;
    QString compiledJsonPath() const;
    QString eventsPath() const;

    // run.json and compiled.json in the on-disk JSON form (4.6). The readers
    // return an empty object when the file is missing or unreadable.
    bool writeState(const QJsonObject &state) const;
    QJsonObject state() const;
    bool writeCompiled(const QJsonObject &compiled) const;
    QJsonObject compiled() const;
    // The workflow document the run started from (Document::toJson()), kept so the worker
    // can hand it to kernels that embed it (RunOptions::document); the file's later edits
    // do not reach it. Empty when the run did not start from a document. Errors: one
    // permission_denied naming the file (writeJsonFile).
    Errors writeWorkflow(const QJsonObject &document) const;
    QJsonObject workflow() const;
    // The file roots the run's nodes are held to (13.6, roots.json), written by `fugue run`;
    // none for a run started without them (not limited).
    Errors writeRoots(const FileRoots &roots) const;
    std::optional<FileRoots> roots() const;
    // The off-machine instances `--allow-remote` allowed for this run alone (allow-remote.json, #404), {"<name>": "<fingerprint>"}
    // (remote_consent.h consentFingerprint of the instance as it was when allowed, so a service changed meanwhile is not covered).
    // Empty when the run was started without. The worker adds the answers kept in remote-consent.json.
    Errors writeRemoteAllowed(const QJsonObject &fingerprints) const;
    QJsonObject remoteAllowed() const;

    // What whoever queues the run writes (9.8): run.json with the status `queued`, and
    // the workflow's path when there is one, then the run.queued event.
    bool queued(const QString &workflow = {}) const;
    // A cancel asked of a run whose process runs it itself (the window, 9.8), which no
    // signal reaches: `fugue runs cancel` leaves the file `cancel` in the folder, and that
    // process looks for it while the run is active.
    bool requestCancel() const;
    bool cancelRequested() const;
    // Ends the run: run.json keeps what it holds and gets `status` and `ended`, then
    // run.done is appended with the errors. Returns that event as it was given to
    // appendEvent, for a caller that also tells listeners. Every way a run ends goes
    // through here: the scheduler's, a cancel of a run that never ran, a dead worker's.
    // A run ends once, across processes: a finish after run.done is there changes nothing
    // and returns that run.done instead.
    QJsonObject finish(RunStatus status, const Errors &errors, const QDateTime &now) const;
    // The run's status: run.done's when the run has ended (it is appended once, by
    // whoever ended it, while run.json is rewritten as the run goes), else run.json's,
    // else what the events so far say, else unknown. The second form saves reading the
    // events again.
    RunStatus status() const;
    RunStatus status(const QList<QJsonObject> &events) const;

    // Appends one event (9.4), adding "run" and "time" (ISO 8601 UTC with
    // milliseconds) when it lacks them. Safe to call from several threads.
    bool appendEvent(const QJsonObject &event) const;
    // Every complete event so far, in order. A torn last line (a crash
    // mid-write) is skipped.
    QList<QJsonObject> events() const;

    // The working folder of the node with this path id, created if needed.
    // Each segment must be a node id ([a-z][a-z0-9_-]{0,31}), so the folder
    // is always inside nodes/.
    Result<QString> nodeDir(const QString &pathId) const;

private:
    RunFolder(QString id, QString path) : m_id(std::move(id)), m_path(std::move(path)) {}

    QString m_id;
    QString m_path;
};

} // namespace fugue
