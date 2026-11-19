#pragma once

// The run folder (spec 9.7): ~/.cache/fugue/runs/<run-id>/ holding
//   run.json       the run's state (its contents belong to the scheduler)
//   compiled.json  the compiled graph that ran
//   events.jsonl   every event, one compact JSON object per line
//   nodes/<path id>/  each node's working folder; a path id's groups become
//                  nested folders, so g7/first is nodes/g7/first/.
//
// Files are written so that another process (`fugue runs status|events`) can
// read them at any time: run.json and compiled.json are replaced atomically,
// and each event is appended as one whole line.

#include <QJsonObject>
#include <QList>
#include <QString>

#include "engine/result.h"

namespace fugue {

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
