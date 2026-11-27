#pragma once

// Canvas actions for the window (#231, docs/canvas-actions-design.md): the task bar's backend, App.canvasActions in QML.
//
// It lists the actions (workflows with an `action` block, engine/canvas_action.h) from its roots, the bundled ones first
// (`:/fugue/actions`, share/actions in the source), each with whether it is broken (and why, in words) and which of its
// models are missing on this machine. Generate (generate()) copies the action's workflow and binds into the copy the
// shown canvas, the full hash of the head that was shown, the selection (a rectangle, or a Quick Mask selection stored
// by content: storeSelection()), the prompt and a seed (random unless one is given), with Canvas Region's padding and
// scale from the router (engine/canvas_router.h); then it queues the copy through RunService as an ordinary run (the
// Queue area lists it under the action's file name, it is cancelled the same way and waits for the one GPU). CanvasRuns
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
// The Advanced popover (slice 4, design 4 and 1.4): advanced(id) lists the inputs the bar has no field of its own for (the
// strength, the model, steps and the plain inputs; not the prompt, seed or layer) and loads the action's workflow into `form`,
// a model of its own that no tab shows, so the popover draws each input with the Graph's InputWidget (the model picker with
// its Download rows, the drag number, the choice) over the same WidgetEdit. An input of the block with `choice_values` is a
// word of its choices (Refine: Light, Default, Restyle) and is set with setChoice(). What the person changes is remembered per
// action and per root in canvas-actions.json (state folder, atomic writes, never inherited by a project) and put into every
// Generate's copy; the file of the action is never written. Count (variations(), 1 to 4) is remembered the same way.
//
// Count N (design 5): one Generate is N runs queued one after the other, all reading the head that was shown, with the
// seeds of one Generate (random each, or fixed s, s+1, ...). Their Region and Text Encode are cache hits. Each lands a layer
// named "<name> (n of N)"; variations 2 to N land hidden (Canvas Write's `visible` and `suffix`). Cancel ends all of them; a
// run that fails ends the rest. runNumber and runTotal say which one is going.
//
// Prompt history (design 4, decision 8): per action, newest first, at most 200, in the root's state folder
// (canvas-prompts.json, atomic writes, never inherited by a project); clearHistory() empties it.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <optional>

#include "canvas/canvas_model.h"
#include "canvas/mask_layer.h"
#include "canvas/task_bar_place.h"
#include "engine/canvas_act.h"
#include "engine/canvas_act_outcome.h"
#include "engine/canvas_action.h"
#include "engine/canvas_batch.h"
#include "engine/document.h"
#include "engine/error.h"
#include "engine/live_link.h"
#include "engine/templates.h"

namespace fugue {

class Registry;
class RunService;
class Workflows;

class CanvasActions : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // [{id, name, description, glyph, selection, needsPrompt, hasLayer (its block has a layer role: it works on the current layer),
    //   broken, reason, missing (models not here), total, download (the button's text, "" when nothing can be fetched), size,
    //   packMissing (a pack its nodes need is not installed), packName, packText ("Needs the Remove Background pack"),
    //   packInstallable (the template says where to get it)}], the bundled first, in name order.
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
    // The live step frame of the run being drawn (docs/canvas-actions-design.md, "Live preview in the selection"): the
    // newest node.preview picture of the active run ("" before the first and after the run), `liveBox` the padded context
    // box it is a picture of and `liveClip` the part of it inside the selection, both {x, y, width, height} in canvas
    // pixels (the canvas `canvas`). `liveFade` says the run ended well, so the frame fades into the new layer; else it goes at once.
    Q_PROPERTY(QString liveFrame READ liveFrame NOTIFY liveChanged)
    Q_PROPERTY(QVariantMap liveBox READ liveBox NOTIFY liveChanged)
    Q_PROPERTY(QVariantMap liveClip READ liveClip NOTIFY liveChanged)
    Q_PROPERTY(bool liveFade READ liveFade NOTIFY liveChanged)
    // The live link (#315, design 16): while `live` is on, a change of the canvas, the selection or the prompt re-runs the chosen action as a
    // preview, once it has been still; the preview shows in the selection (liveFrame) and writes nothing. Generate then keeps it.
    Q_PROPERTY(bool live READ live NOTIFY liveStateChanged)
    Q_PROPERTY(bool liveBusy READ liveBusy NOTIFY liveStateChanged) // a preview run is waiting or active
    // Which run of the Generate is going or next ("Generating 2 of 3") and how many it made (1 when Count is 1), 0 when idle.
    Q_PROPERTY(int runNumber READ runNumber NOTIFY stateChanged)
    Q_PROPERTY(int runTotal READ runTotal NOTIFY stateChanged)
    // The model the Advanced popover's InputWidgets edit (see advanced()).
    Q_PROPERTY(CanvasModel *form READ form CONSTANT)
    // Where the person left the task bar (#381), kept in the state folder.
    Q_PROPERTY(fugue::TaskBarPlace *barPlace READ barPlace CONSTANT)
    // A download of an action's models is under way, and its line ("flux-2-klein-4b-Q8_0.gguf, 1 of 3 · 1.2 of 8.9 GB").
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadsChanged)
    Q_PROPERTY(QString downloadText READ downloadText NOTIFY downloadsChanged)

public:
    // `workflows` gives the missing models and the download line; it may be null (no download offer then).
    CanvasActions(RunService *service, const Registry *registry, Workflows *workflows, QObject *parent = nullptr);

    static constexpr int historyLimit = 200;
    static constexpr int maxVariations = 4;
    static QString bundledRoot() { return QStringLiteral(":/fugue/actions"); }

    // The folders of action files (*.fugue), in order; an id found twice is the first one's. Default: bundledRoot().
    void setRoots(const QStringList &roots);
    // canvas-prompts.json; default <stateDir>/canvas-prompts.json.
    void setHistoryPath(const QString &path);
    // canvas-actions.json; default <stateDir>/canvas-actions.json.
    void setSettingsPath(const QString &path);

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
    CanvasModel *form() const { return m_form; }
    TaskBarPlace *barPlace() const { return m_barPlace; }
    int runNumber() const;
    int runTotal() const { return busy() ? int(m_order.size()) : 0; }
    QString liveFrame() const { return m_liveFrame; }
    QVariantMap liveBox() const { return m_liveBox; }
    QVariantMap liveClip() const { return m_liveClip; }
    bool liveFade() const { return m_liveFade; }
    bool downloading() const;
    QString downloadText() const;
    bool live() const { return m_link.enabled(); }
    bool liveBusy() const { return !m_liveRun.isEmpty(); }
    // The quietest the link waits (milliseconds; 600 by default). For tests.
    void setLiveQuiet(qint64 ms) { m_link.setBase(ms); }

    // Generate. `request`: {action ("auto" or an id), canvas, revision (the full hash shown), prompt, rect {x, y, width,
    // height} (canvas pixels; with `selection` the selection's bounds), selection (a stored selection's hash), seed (a
    // number: fixed; none: random), layer (the name of one layer Canvas Region reads instead of the whole picture, #226)}. {ok, run, action, reason}: `reason` (also `error`) says in words why nothing started.
    Q_INVOKABLE QVariantMap generate(const QVariantMap &request);
    // The live link (design 16). setLive(true) picks the seed every preview and the Generate that keeps it use (so the preview is the
    // picture that lands; liveReroll() picks another). The bar calls liveNote() on every change of what a run reads: `key` names it
    // all (canvas, revision, action, prompt, selection, layer, ...), `revision` is the revision shown, `ready` says a run may start
    // (a selection, no blocker, nothing else generating). When the state has been still, liveDue() asks the bar to start the preview:
    // generate() with `live: true`, the same request a Generate makes. A preview that fails, or that nobody starts, switches live off.
    Q_INVOKABLE void setLive(bool on);
    Q_INVOKABLE void liveNote(const QString &key, const QString &revision, bool ready);
    Q_INVOKABLE void liveReroll();
    // `count` (1 to 4): the runs of this Generate; none: the action's remembered Count. An action with no seed role makes one.
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
    // Asks the window to install the pack the action needs (its template carries it, or says where it is): Settings > Packs shows what it
    // is and asks before it installs and before it is trusted (6.6, 6.7). False when nothing says where to get it. Nothing is fetched here.
    Q_INVOKABLE bool installPack(const QString &action);
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

    // The Advanced popover's rows for an action ("auto" is the one Auto falls back to): [{key, label, kind ("widget": an
    // InputWidget of `form` at node.input; "choice": a word of `choices`), node, input, choices, value (the word), note}].
    // Loads `form` with the action's workflow and the remembered values. Empty for an action that is broken or has none.
    Q_INVOKABLE QVariantList advanced(const QString &id);
    // The word a choice input stands at: the remembered one, else the block's default; empty for an input that is none.
    Q_INVOKABLE QString choiceOf(const QString &id, const QString &key) const;
    // A choice input's word, remembered; the default word forgets it. An unknown word is refused: false.
    Q_INVOKABLE bool setChoice(const QString &id, const QString &key, const QString &word);
    // Forgets the remembered Advanced values of the action (not Count); the form reloads.
    Q_INVOKABLE void resetAdvanced(const QString &id);
    // Whether the action has remembered Advanced values.
    Q_INVOKABLE bool hasAdvancedChanges(const QString &id) const;
    // Count: 1 to 4, remembered per action; 1 for an action with no seed role (the runs would come out the same).
    Q_INVOKABLE int variations(const QString &id) const;
    Q_INVOKABLE void setVariations(const QString &id, int count);

    Q_INVOKABLE QStringList history(const QString &action) const;
    Q_INVOKABLE void clearHistory();

    // A run's error as the bar says it (design 4), for an action named `actionName`.
    static QString plainWords(const Error &error, const QString &actionName);

signals:
    void actionsChanged();
    void stateChanged();
    void progressChanged();
    void liveChanged();
    void downloadsChanged();
    void historyChanged();
    void liveStateChanged();
    // The live link wants a preview now: the bar answers with generate({... live: true}).
    void liveDue();
    // A run of a Generate ended: "ok", "failed" or "cancelled".
    void finished(const QString &run, const QString &status);
    // An action whose result is a new canvas (Upscale: Canvas New) finished: the canvas it ran on and the one it made. The view that
    // shows `source` switches to `made`; the source itself is as it was.
    void canvasMade(const QString &source, const QString &made);

private:
    using Entry = canvas::ActionEntry;

    const Entry *entry(const QString &id) const;
    void rebuildList();
    void onEvent(const QString &run, const QJsonObject &event);
    void endRun(const QString &run, const QString &status);
    void setError(const QString &line, const QString &code, const QString &action = {});
    QVariantMap refuse(const QString &why);
    void remember(const QString &action, const QString &prompt);
    QJsonObject readSettings() const;
    void writeSettings(const QJsonObject &actions);
    QJsonObject settingsOf(const QString &id) const; // {values, count}
    void storeSettings(const QString &id, const QJsonObject &entry);
    QString resolved(const QString &id) const; // "auto" and "" as the action Auto falls back to
    // The action's workflow with the remembered values put in (a choice input's word stays a word).
    Document withRemembered(const Entry &entry, const QJsonObject &values) const;
    void syncForm();
    // The inputs of the block the popover lists.
    static bool inPopover(const canvas::ActionInput &input);
    QJsonObject readHistory() const;

    RunService *m_service;
    TaskBarPlace *m_barPlace = nullptr;
    const Registry *m_registry;
    Workflows *m_workflows;
    QStringList m_roots;
    QString m_historyPath, m_settingsPath;
    CanvasModel *m_form = nullptr; // the popover's own, owned by this
    QString m_formAction;
    bool m_loadingForm = false;
    QStringList m_order; // the runs of the last Generate, in order
    QList<Entry> m_entries;
    QVariantList m_list;

    QStringList m_runs; // waiting and active, in the order started
    QString m_active;
    QString m_canvas;
    QString m_action;
    QString m_actionName;
    qreal m_progress = -1;
    QString m_error, m_errorCode, m_errorAction, m_notice, m_lastRun;
    // Per run, where its pictures go: the context box and the selection inside it (canvas pixels), made when it starts.
    struct LiveRect {
        QVariantMap box, clip;
    };
    QHash<QString, LiveRect> m_liveRects;
    QString m_liveFrame;
    QVariantMap m_liveBox, m_liveClip;
    bool m_liveFade = false;
    void clearLive(bool fade);
    // The live link's preview run.
    QVariantMap generateNow(const QVariantMap &request, bool live);
    QVariantMap startLive(const Entry &chosen, const canvas::ActPlan &plan, const QString &canvasId);
    void onLiveEvent(const QJsonObject &event);
    void endLive(const QString &status, const Errors &errors);
    void liveStep();
    void detachLive();
    qint64 liveNow() const { return m_liveClock.elapsed(); }
    LiveLink m_link;
    QElapsedTimer m_liveClock;
    QTimer m_liveTimer;
    QString m_liveRun, m_liveTarget;
    Errors m_liveErrors;
    qint64 m_liveSeed = 0;
    QString m_noteKey, m_noteRevision;
    QSet<QString> m_ownRevisions; // revisions this runner's Generates landed: not a change the person made
    canvas::ActOutcome m_outcome; // what this Generate's runs came to: the layers, the canvases made, the errors, Expand's growth
};

} // namespace fugue
