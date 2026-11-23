#pragma once

// Canvas actions for the window (#231, docs/canvas-actions-design.md): the task bar's backend, App.canvasActions in QML.
//
// It lists the actions (workflows with an `action` block, engine/canvas_action.h) from its roots, the bundled ones first
// (`:/fugue/actions`, share/actions in the source), each with whether it is broken (and why, in words) and which of its
// models are missing on this machine. Generate (generate()) copies the action's workflow and binds into the copy the
// shown canvas, the full hash of the head that was shown, the selection (a rectangle, or a Quick Mask selection stored
// by content: storeSelection()), the prompt and a seed (random unless one is given), with Canvas Region's padding and
// scale from the router (engine/canvas_router.h); then it queues the copy through RunService as an ordinary run (the
// queue panel lists it under the action's file name, it is cancelled the same way and waits for the one GPU). CanvasRuns
// ignores these runs: they belong to no tab. "auto" lets the router pick the action from the selection and the prompt;
// in this slice only Generative fill exists, so a route to another action says so in words.
//
// While its runs wait or run, `busy` is true, `canvas` names the canvas they are for (a bar of another canvas shows
// nothing of them), `progress` is the running node's (the Sampler's) 0..1 or -1, and cancel() ends them: a cancelled
// run writes nothing and says nothing. When a run ends, `error` is one plain line (design 4: an empty selection, no
// model that can run it, not enough GPU memory, a picture of another shape, a canvas that changed size; otherwise the
// error's own sentence), and `errorAction` names the action whose missing models the line offers to download.
//
// A fully cached Write (design decision 5): the same prompt, seed, selection and head after an undo is a cache hit all
// the way, Canvas Write included, so the run lands nothing. When its run ends, a Write that came from the cache has the
// revision it landed placed on the head again (CanvasStore::accept: the head moves to it when it was made from the head,
// else its added layers are placed on the head, the "layers only add" rule). When that revision is still on the head,
// `notice` says so ("Same prompt, seed and selection as …"): correct, not a fault.
//
// Where the actions come from: the template library (a template whose workflow has a valid `action` block is an action,
// the person's own and a project's first; nothing else indexes them), then the files compiled into Fugue, which a template
// of the same id replaces. A plain template is not an action and never hides one.
//
// Prompt history (design 4, decision 8): per action, newest first, at most 200, in the root's state folder
// (canvas-prompts.json, atomic writes, never inherited by a project); clearHistory() empties it.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <optional>

#include "canvas/mask_layer.h"
#include "engine/canvas_action.h"
#include "engine/canvas_batch.h"
#include "engine/document.h"
#include "engine/error.h"

namespace fugue {

class Registry;
class RunService;
class Workflows;

class CanvasActions : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // [{id, name, description, glyph, selection, needsPrompt, broken, reason, missing (models not here), total,
    //   download (the button's text, "" when nothing can be fetched), size}], the bundled first, in name order.
    Q_PROPERTY(QVariantList actions READ actions NOTIFY actionsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)       // runs of the last Generate waiting or active
    Q_PROPERTY(bool running READ running NOTIFY stateChanged) // one of them is active
    Q_PROPERTY(int count READ count NOTIFY stateChanged)      // how many are left
    Q_PROPERTY(QString canvas READ canvas NOTIFY stateChanged)
    Q_PROPERTY(QString action READ action NOTIFY stateChanged) // the action they run (its id)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY stateChanged)
    Q_PROPERTY(QString errorAction READ errorAction NOTIFY stateChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY stateChanged)
    Q_PROPERTY(QString lastRun READ lastRun NOTIFY stateChanged)
    // A download of an action's models is under way, and its line ("flux-2-klein-4b-Q8_0.gguf, 1 of 3 · 1.2 of 8.9 GB").
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadsChanged)
    Q_PROPERTY(QString downloadText READ downloadText NOTIFY downloadsChanged)

public:
    // `workflows` gives the missing models and the download line; it may be null (no download offer then).
    CanvasActions(RunService *service, const Registry *registry, Workflows *workflows, QObject *parent = nullptr);

    static constexpr int historyLimit = 200;
    static QString bundledRoot() { return QStringLiteral(":/fugue/actions"); }

    // The folders of action files (*.fugue), in order; an id found twice is the first one's. Default: bundledRoot().
    void setRoots(const QStringList &roots);
    // canvas-prompts.json; default <stateDir>/canvas-prompts.json.
    void setHistoryPath(const QString &path);

    QVariantList actions() const { return m_list; }
    bool busy() const { return !m_runs.isEmpty(); }
    bool running() const { return !m_active.isEmpty(); }
    int count() const { return int(m_runs.size()); }
    QString canvas() const { return m_canvas; }
    QString action() const { return m_action; }
    qreal progress() const { return m_progress; }
    QString error() const { return m_error; }
    QString errorCode() const { return m_errorCode; }
    QString errorAction() const { return m_errorAction; }
    QString notice() const { return m_notice; }
    QString lastRun() const { return m_lastRun; }
    bool downloading() const;
    QString downloadText() const;

    // Generate. `request`: {action ("auto" or an id), canvas, revision (the full hash shown), prompt, rect {x, y, width,
    // height} (canvas pixels; with `selection` the selection's bounds), selection (a stored selection's hash), seed (a
    // number: fixed; none: random), layer (the name of one layer Canvas Region reads instead of the whole picture, #226)}. {ok, run, action, reason}: `reason` (also `error`) says in words why nothing started.
    Q_INVOKABLE QVariantMap generate(const QVariantMap &request);
    // Ends this bar's runs, the waiting ones first.
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearError();
    // The Quick Mask selection kept by content in the session's canvas store (CanvasStore::storeSelection): {ok, hash,
    // x, y, width, height (its bounds)} or {ok: false, reason}.
    Q_INVOKABLE QVariantMap storeSelection(fugue::MaskLayer *selection) const;
    // The bounds of what is painted in the selection, {x, y, width, height}; {} when nothing is (the bar is placed by it).
    Q_INVOKABLE QVariantMap selectionBounds(fugue::MaskLayer *selection) const;
    // Fetches the action's missing models through the window's download line (a template's Download).
    Q_INVOKABLE void download(const QString &action);
    // Edit workflow (design 7): the action's workflow as a tab of the Graph, a copy no file holds (Save asks where; a
    // bundled action is never written). "auto" is the action Auto falls back to. {ok, action, name} or {ok: false, reason}.
    // The window then shows the Graph (the host's job: it knows its areas).
    Q_INVOKABLE QVariantMap editWorkflow(const QString &id);
    // Reads the actions and their models again.
    Q_INVOKABLE void refresh();
    // The action "auto" stands for with this selection and prompt: {action (an id or ""), reason}.
    Q_INVOKABLE QVariantMap route(const QString &canvas, const QString &revision, const QVariantMap &rect, bool hasPrompt) const;

    // The action to run over a folder (canvas/canvas_batch.h, #250): a copy of the listed action `id`, or why it cannot be
    // batched (it is broken, it needs a crop frame, or its models are not on this machine: the code is then `model_missing`).
    Result<canvas::BatchAction> batchAction(const QString &id) const;

    Q_INVOKABLE QStringList history(const QString &action) const;
    Q_INVOKABLE void clearHistory();

    // A run's error as the bar says it (design 4), for an action named `actionName`.
    static QString plainWords(const Error &error, const QString &actionName);

signals:
    void actionsChanged();
    void stateChanged();
    void progressChanged();
    void downloadsChanged();
    void historyChanged();
    // A run of a Generate ended: "ok", "failed" or "cancelled".
    void finished(const QString &run, const QString &status);

private:
    struct Entry {
        QString id;
        QString file;
        bool library = false; // from the template library, else compiled into Fugue
        Document document;
        std::optional<canvas::Action> action;
        canvas::ActionGraph graph;
        QString broken; // the reason it cannot run, in words
    };
    struct WriteResult {
        bool cached = false;
        QString canvas, revision;
    };

    const Entry *entry(const QString &id) const;
    void rebuildList();
    void onEvent(const QString &run, const QJsonObject &event);
    void endRun(const QString &run, const QString &status, const Errors &errors);
    // A Write that came from the cache: its revision placed on the head again (see the header).
    void placeAgain(const WriteResult &write);
    void setError(const QString &line, const QString &code, const QString &action = {});
    QVariantMap refuse(const QString &why);
    void remember(const QString &action, const QString &prompt);
    QJsonObject readHistory() const;

    RunService *m_service;
    const Registry *m_registry;
    Workflows *m_workflows;
    QStringList m_roots;
    QString m_historyPath;
    QList<Entry> m_entries;
    QVariantList m_list;

    QStringList m_runs; // waiting and active, in the order started
    QString m_active;
    QString m_canvas;
    QString m_action;
    QString m_actionName;
    qreal m_progress = -1;
    QString m_error, m_errorCode, m_errorAction, m_notice, m_lastRun;
    QHash<QString, QList<WriteResult>> m_writes; // per run, its Writes' landed revisions
    QHash<QString, Errors> m_nodeErrors;         // per run, its node errors so far
};

} // namespace fugue
