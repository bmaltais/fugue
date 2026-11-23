#pragma once

// The Canvas view's Batch… dialog's backend (#250), App.canvasBatch in QML: a canvas action over a folder of pictures,
// run in the background through the window's RunService. The steps are engine/canvas_batch.h's (the same as `fugue
// canvas batch`): batchPrepare imports a picture as a canvas and binds the action to it, a run of the bound workflow
// goes through the queue (it waits for the one GPU like any run, and shows in the queue panel), and batchFinish exports
// the head the run left. One picture at a time; the window stays free.
//
// start() begins; `results` lists every picture with its status ("waiting", "running", "ok", "failed", "cancelled"),
// the file written and the reason in words, and grows as pictures end; a picture that fails never stops the batch.
// cancel() ends the run under way (nothing is written for it) and the pictures not started are marked cancelled.
// `summary` is the one line at the end ("3 pictures done, 1 failed"). reset() clears a finished batch for the next one.

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "engine/canvas_batch.h"

namespace fugue {

class CanvasActions;
class RunService;

class CanvasBatch : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool finished READ finished NOTIFY stateChanged) // a batch ended and its results are shown
    Q_PROPERTY(int total READ total NOTIFY stateChanged)
    Q_PROPERTY(int done READ done NOTIFY stateChanged)          // pictures that ended, whatever the status
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged) // the picture under way: its Sampler's 0..1, or -1
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY stateChanged)
    Q_PROPERTY(QString outFolder READ outFolder NOTIFY stateChanged)

public:
    CanvasBatch(RunService *service, CanvasActions *actions, QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    bool finished() const { return m_finished; }
    int total() const { return int(m_files.size()); }
    int done() const { return m_done; }
    qreal progress() const { return m_progress; }
    QVariantList results() const { return m_results; }
    QString summary() const { return m_summary; }
    QString outFolder() const { return m_spec.outFolder; }

    // The actions the dialog offers: the listed ones that are not broken and need no crop frame, {id, name, description,
    // needsPrompt, missing (models not here)}, in the action list's order.
    Q_INVOKABLE QVariantList actions() const;
    // How many pictures a folder holds (a path or a file: URL); -1 when it cannot be read.
    Q_INVOKABLE int pictureCount(const QString &folder) const;
    // {action (an id), inFolder, outFolder (paths or file: URLs), format ("png", "jpg", "webp", "ora"), quality (1..100),
    //  prompt}. {ok} or {ok: false, reason} (in words) when nothing started.
    Q_INVOKABLE QVariantMap start(const QVariantMap &request);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reset();

signals:
    void stateChanged();
    void progressChanged();
    void resultsChanged();
    // One picture ended: its index, and "ok", "failed" or "cancelled".
    void itemDone(int index, const QString &status);

private:
    void onEvent(const QString &run, const QJsonObject &event);
    void next();
    void endItem(int index, const QString &status, const QString &output, const QString &error);
    void conclude();
    void setStatus(int index, const QString &status);

    RunService *m_service;
    CanvasActions *m_actions;
    canvas::BatchSpec m_spec;
    QStringList m_files;
    QSet<QString> m_taken;
    QVariantList m_results;
    int m_index = -1; // the picture under way
    int m_done = 0;
    canvas::BatchPrepared m_prepared;
    QString m_run;
    Errors m_errors; // the run's node errors so far
    qreal m_progress = -1;
    bool m_busy = false;
    bool m_finished = false;
    bool m_cancelling = false;
    QString m_summary;
};

} // namespace fugue
