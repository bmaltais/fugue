#pragma once

// The App view's store (Fugue Apps, #385, epic #397, spec 4.9 and 12.17): the shown workflow as one simple screen, with no
// window in it. A document with an `app` block (engine/app_binding.h) is a form (inputs), one Run, an output area, Cancel
// and progress, a quiet Details for the run's errors, and Open in Graph. The class is the logic and QML (src/apps/AppPage.qml)
// only draws it: value in (setValue), change signal out.
//
// Tab mode: until #217's view registry carries it, the App view is a mode of a workflow's tab: one area, the window's body.
// `shown` is true when the shown tab's workflow has an app block that reads and the person has not opened it in the graph; the
// window then draws the page in the canvas's place, and a workflow with no block never changes. A tab opens in the view its block's
// `defaultView` names; openInGraph() and showApp() switch this tab, and the choice is kept per tab for the session.
//
// Runs: Run binds the form's values into a copy of the document (app::bindApp: the tab's document and file are never
// changed) and hands it to the injected `start`, which is RunService::start in the window, so the run waits in the one queue
// every run waits in. The window's events come back through onEvent(run, event); a run belongs to its tab, so a tab switched
// away from keeps its progress and results and shows them again. The app owns no outputs: the results of the app's output
// nodes (their thumbnails and files, the ones the Graph's cards show) are what the last run of the tab made, in the order the
// block lists them; run history and the results store are the existing ones: the takes of an app with a file are read back from the
// run folders (apps/app_takes.h, #389), so they survive a restart; an app with no file keeps the session's only.
//
// Live (#471): per tab and for the session, "off" at first, "live" (an input change queues a run once it has been still for
// liveDelay, ~600 ms) or "repeat" (a run ends, the next starts, until Cancel or a failure). Nothing is armed while it is off.
//
// Words: nothing here names a node or a graph port. An output is called by its label, else "Result". Errors are shown as
// the engine words them; `error` is the first line, `details` all of them (with their hint).
//
// Fields (`fields`): one map per input in the form's order: {key, label, kind, value, min?, max?, choices?, section, advanced}
// and what its kind adds. The kind is the control (#386, AppInput.qml draws one per kind):
//   "text"    a prompt or a plain text, always a wrapping box that grows to 8 lines; `multiline` when it takes newlines (the prompt
//             roles, a target with the multiline widget), `minLines` its first height (3 for a prompt, else 1)
//   "number"  a strength, a steps, a number value; a drag field with `min` and `max`, else a text field; "" is the workflow's own
//   "toggle"  a switch (a boolean in the workflow)
//   "choice"  `choices` [{value, text}]: a block's choices
//   "seed"    a number with a `mode` ("fixed", "random", "increment"), as a node's seed has: random binds a new seed for each
//             run, increment binds the value and adds one after the run starts (valueChanged says it), fixed binds the value
//   "image"   a file path; `samples` [{path, url, name}] are the template's inputs/ pictures; drop, paste and pick take a picture
//             into the inputs folder; recentImages() lists the last ones used
//   "mask"    the path of a painted mask over the form's first image (maskPicture)
//   "model"   a handle; `resource` is its type, models(key) lists what can be chosen, with the Recommended one marked
// A value starts as the form's last value, else the block's default, else what the workflow has in the first target. The list
// is rebuilt only when the app changes, never by typing, so a field keeps its focus; a value the store changes by itself
// (an image taken, a seed advanced, a model hashed) comes as valueChanged(key).

#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QMimeData>
#include <QObject>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>
#include <optional>

#include "apps/needs_model.h"
#include "engine/app_binding.h"
#include "engine/document.h"

namespace fugue {

class AppModels;
class ModelDownloads;
class Registry;
struct RunRequest; // run/run_service.h: only the .cpp needs it, so this header (in qml_app.h) stays light

class AppView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // The shown workflow has an app block that reads (the shown tab can be an app).
    Q_PROPERTY(bool isApp READ isApp NOTIFY appChanged)
    // The window shows the App view: isApp and not opened in the graph.
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)
    Q_PROPERTY(QString title READ title NOTIFY appChanged)
    // The workflow file the form was made from (a template's own workflow.fugue, else the saved file), empty for an unsaved one.
    Q_PROPERTY(QString file READ file NOTIFY appChanged)
    // The picture of the app (preview.webp|png|jpg beside its workflow: a template's), empty when there is none.
    Q_PROPERTY(QUrl preview READ preview NOTIFY appChanged)
    // The moving preview (a WebM beside the workflow: template.json's `preview`, 11.10) played in the header; empty when none.
    Q_PROPERTY(QUrl previewVideo READ previewVideo NOTIFY appChanged)
    Q_PROPERTY(QString description READ description NOTIFY appChanged)
    Q_PROPERTY(QVariantList fields READ fields NOTIFY appChanged)
    // Why Run cannot (the block does not fit the workflow), in one line; empty when it can.
    Q_PROPERTY(QString problem READ problem NOTIFY appChanged)
    // "idle", "waiting", "running", "done", "failed" or "cancelled": how this tab's last run is going or ended.
    Q_PROPERTY(QString status READ status NOTIFY runChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY runChanged)       // a run of this tab waits or goes
    Q_PROPERTY(bool canRun READ canRun NOTIFY runChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged) // of the step going, 0 to 1, or -1
    Q_PROPERTY(QString error READ error NOTIFY runChanged)
    // [{message, hint}] of the last run, the quiet Details.
    Q_PROPERTY(QVariantList details READ details NOTIFY runChanged)
    // [{label, kind ("image", "video", "audio" or "text"), url (the picture to draw), full (its file as a URL), width, height,
    //   text}] of the last run, in the block's order.
    Q_PROPERTY(QVariantList results READ results NOTIFY runChanged)
    // The takes of this app, newest first (#389): this tab's runs of the session, then the runs History has for the app (apps/app_takes.h):
    // [{run, status, pending, url (the first picture, or empty), count (results), line (what the form had, in a quiet line), shown,
    //   persisted}].
    Q_PROPERTY(QVariantList takes READ takes NOTIFY runChanged)
    // "off", "live" or "repeat" (#471): this tab's Live mode.
    Q_PROPERTY(QString liveMode READ liveMode NOTIFY liveChanged)
    // The take the stage shows, as an index into `takes`, or -1. The newest take that made something, unless the person picked
    // an older one (selectTake); a run that is still waiting has made nothing yet, so the stage keeps the one before it.
    Q_PROPERTY(int takeIndex READ takeIndex NOTIFY runChanged)
    // The shown take's results (the same shape as `results`) and its quiet line of inputs.
    Q_PROPERTY(QVariantList shownResults READ shownResults NOTIFY runChanged)
    Q_PROPERTY(QString shownInputs READ shownInputs NOTIFY runChanged)
    // The newest step picture of the run going ("" when none): what a Sampler's preview gives while it works.
    Q_PROPERTY(QString liveFrame READ liveFrame NOTIFY progressChanged)
    // Something is shown of a finished run (results, an error) or the strip has takes, and no run of this tab goes: Clear can act.
    // Clear empties the stage and the strip; the run folders stay (History, Results), and the strip's old takes stay away after a restart.
    Q_PROPERTY(bool canClear READ canClear NOTIFY runChanged)
    // The app has a seed input, so Repeat can make something new each run (owner, 2026-11-25: Repeat on Upscale, which has none,
    // made a strip of identical takes in a second: each run came from the cache at once). Without one the form offers no Repeat.
    Q_PROPERTY(bool canRepeat READ canRepeat NOTIFY appChanged)
    // Repeat is chosen but no seed is Random or Increment, so the next run would be the same: it waits (the form says why) and goes on
    // once a seed is set to vary.
    Q_PROPERTY(bool repeatWaits READ repeatWaits NOTIFY liveChanged)
    // What the app still needs (a missing model or pack), listed while it is not ready (#469).
    Q_PROPERTY(fugue::NeedsModel *needs READ needs CONSTANT)
    // The paired host(s) the app runs on, when its models are not on this machine and the paired verdict says so (#395, #581); empty when
    // it runs here. The form says "Runs on <host>" near Run.
    Q_PROPERTY(QString runsOn READ runsOn NOTIFY runsOnChanged)
    // The paired host that did not answer, where it is the only one that could take the app (#581): the form says "<host> is not
    // answering" on the same line. Empty otherwise, and while a service off this machine is named.
    Q_PROPERTY(QString silentHost READ silentHost NOTIFY runsOnChanged)

public:
    using Start = std::function<QString(const RunRequest &)>;
    using Cancel = std::function<void(const QString &run)>;
    // Asks for a finished run's results again; they come back through onEvent (RunService::replay).
    using Replay = std::function<void(const QString &run)>;

    // `registry` prunes the block of what the workflow no longer has and checks it; none: the block is used as it is.
    AppView(Start start, Cancel cancel, const Registry *registry = nullptr, QObject *parent = nullptr);

    // The tab the window shows and its workflow (`file`: where it came from, or empty). Called when the tab changes and
    // when the person comes back to the view, so edits made in the graph are seen. A tab seen for the first time opens in
    // its block's defaultView.
    // `templateFolder`: where a tab of a template with no file of its own came from; its workflow.fugue is then the app's key for
    // its takes (a saved file is its own key).
    void show(const QString &tab, const Document &document, const QString &file, const QString &templateFolder = {});
    // The tab was closed: what it kept goes (its run is cancelled).
    void forget(const QString &tab);
    // Where the run folders are and how to read a run's results again: the takes of an app with a file are read from there
    // (#389). Without it the takes are the session's.
    void setHistory(const QString &runsRoot, Replay replay);
    // Live's debounce in ms (600 at first; tests shorten it).
    void setLiveDelay(int milliseconds) { m_liveTimer.setInterval(milliseconds); }
    QString liveMode() const { return m_state.value(m_tab).live; }
    // "off", "live" or "repeat". Repeat starts a run at once when none goes, and the next when one ends while a seed varies (Random
    // or Increment); it is refused for an app with no seed (canRepeat). Cancel turns it off.
    Q_INVOKABLE void setLiveMode(const QString &mode);
    // Whether anything is armed for Live (a debounce waiting): false at idle and when it is off.
    bool liveArmed() const { return m_liveTimer.isActive(); }

    bool isApp() const { return m_app.has_value(); }
    // With one area, the tab's mode; in a split (setAreas), whether the active area shows the App view.
    bool shown() const { return m_app && (m_split ? m_areaApp : !m_state[m_tab].graph); }
    // The window's areas (#450): `split` when the window has several, `appActive` when the active one shows the App view. In a split
    // the area's view type decides what `shown` is, and the tab's own mode is kept for the one-area window.
    Q_INVOKABLE void setAreas(bool split, bool appActive);
    QString title() const { return m_app ? m_app->title : QString(); }
    QString file() const { return m_file; }
    QUrl preview() const;
    QUrl previewVideo() const;
    QString description() const { return m_app ? m_app->description : QString(); }
    QVariantList fields() const { return m_fields; }
    QString problem() const { return m_problem; }
    QString status() const;
    bool busy() const;
    bool canRun() const { return m_app && m_problem.isEmpty() && !busy(); }
    qreal progress() const { return m_state.value(m_tab).progress; }
    QString error() const { return m_state.value(m_tab).error; }
    QVariantList details() const { return m_state.value(m_tab).details; }
    QVariantList results() const { return m_state.value(m_tab).results; }
    QVariantList takes() const;
    int takeIndex() const;
    QVariantList shownResults() const;
    QString shownInputs() const;
    QString liveFrame() const { return m_state.value(m_tab).liveFrame; }
    bool canClear() const;
    bool canRepeat() const;
    bool repeatWaits() const;

    // A form's value, by the field's key. Empty text or NaN for a number takes the value away (the workflow's own stays).
    Q_INVOKABLE void setValue(const QString &key, const QVariant &value);
    // The form's value now, by key: what the field shows (a seed in random mode has none).
    Q_INVOKABLE QVariant valueOf(const QString &key) const;
    // The form's values as they would be bound now, by key.
    QMap<QString, QJsonValue> values() const;
    // A seed's mode: "fixed", "random" or "increment". Fixed and increment need a number: with none shown, one is picked.
    Q_INVOKABLE void setSeedMode(const QString &key, const QString &mode);
    Q_INVOKABLE QString seedMode(const QString &key) const;
    // A fresh number in the seed's range, as the value of a fixed or incrementing seed (the dice).
    Q_INVOKABLE void rollSeed(const QString &key);

    // Images (#386). Pictures are taken into the inputs folder as Load Image takes them (workflow/image_inputs.h): the same bytes
    // are one file. Each returns why not, or empty. The value becomes the stored file's path.
    Q_INVOKABLE QString setImageFiles(const QString &key, const QList<QUrl> &files);
    QString setImageFromMime(const QString &key, const QMimeData &data); // what a paste or a drop offers
    Q_INVOKABLE QString pasteImage(const QString &key); // from the clipboard
    Q_INVOKABLE bool canPasteImage() const;
    // Ctrl+V and Edit > Paste while the App view is shown: the clipboard's picture goes into the focused picture input (the
    // last one that took focus, setFocusedImage), else the form's first. False, doing nothing, when the clipboard has no
    // picture or the form has no picture input (the caller then lets the key mean what it means elsewhere). The outcome (a
    // refusal, or none) comes as imageNotice.
    bool pasteToApp();
    // A picture dropped anywhere on the page: the same target; returns why not, or empty.
    Q_INVOKABLE QString dropImageFiles(const QList<QUrl> &files);
    Q_INVOKABLE void setFocusedImage(const QString &key, bool focused);
    // A file already in the inputs folder, or one of the template's samples.
    Q_INVOKABLE void useImage(const QString &key, const QString &path);
    // The images last used: {path, url, name}, newest first.
    Q_INVOKABLE QVariantList recentImages() const;
    // What a path in a field is as a URL to draw (a relative one is the workflow file's folder's); empty for none.
    Q_INVOKABLE QUrl fileUrl(const QString &path) const;
    // The same as a path (a relative one against the workflow file's folder); empty for none.
    Q_INVOKABLE QString resolvedPath(const QString &path) const;
    // The picture a mask is painted over: the first image input's now: {url, width, height}; empty when none is chosen.
    Q_INVOKABLE QVariantMap maskPicture() const;
    // The picture the shown take ran with ({url, width, height}, the block's `compare.input`, else the form's first picture), or
    // empty when the take has none on disk.
    Q_INVOKABLE QVariantMap takeInputPicture() const;
    // Where the input picture sits in a result of the shown take (Compare, the block's `compare`): a rect in the result's
    // pixels, from the take's own values (what was run, not the form now). Empty when the block has no `compare`, the take has no
    // picture on disk, or a size is unknown.
    Q_INVOKABLE QRectF inputPlacement(int resultWidth, int resultHeight) const;

    // Models (#386): what a model field can be given. request starts the lists (disk and catalog at once, providers and the
    // Recommended mark when they answer: modelsChanged), models gives them (AppModels::entries), pickModelFile takes a file on
    // disk (its hash is read first when it is not known: `waitingModels` has the key meanwhile), pickModel a listed handle,
    // downloadModel a catalog model, which becomes the value when it has arrived. The last two return why not, or empty.
    Q_INVOKABLE void requestModels(const QString &key);
    Q_INVOKABLE QVariantList models(const QString &key) const;
    Q_INVOKABLE QString modelsEmptyText(const QString &key) const;
    Q_INVOKABLE bool modelsLoading(const QString &key) const;
    Q_INVOKABLE void pickModel(const QString &key, const QVariantMap &handle);
    Q_INVOKABLE void pickModelFile(const QString &key, const QString &path);
    Q_INVOKABLE QVariantMap catalogInfo(const QString &key, const QString &id) const;
    Q_INVOKABLE QString downloadModel(const QString &key, const QString &id);
    Q_PROPERTY(QStringList waitingModels READ waitingModels NOTIFY modelsChanged)
    QStringList waitingModels() const; // this tab's fields only
    // The window's one download line, for the Download of a model field.
    void setDownloads(ModelDownloads *downloads);
    // The model lists (a test gives them folders and a provider).
    AppModels *modelStore() const { return m_models; }
    NeedsModel *needs() const { return m_needs; }
    const Registry *registry() const { return m_registry; }

    // Where the app runs (#395, #581): the window gives the paired verdict's hosts (comma separated) and the silent host, or none of
    // them. A tab shown again clears them until the verdict is made again.
    void setPairedHosts(const QString &runsOn, const QString &silentHost);
    // The service off this machine the shown app's nodes are routed to ("<service> (<host>)", 8.2, #404): said by whoever checks the
    // graph (CanvasRuns::runsOn). It takes the place of the paired host in `runsOn`, the same "Runs on" line.
    void setOffMachine(const QString &services);
    QString runsOn() const { return m_offMachine.isEmpty() ? m_runsOn : m_offMachine; }
    QString silentHost() const { return m_offMachine.isEmpty() ? m_silentHost : QString(); }

    // Queues a run of the shown tab's workflow with the form's values in. Returns its id, empty when it cannot run
    // (`error` says why).
    Q_INVOKABLE QString run();
    Q_INVOKABLE void cancel();
    // Takes (#389). The stage shows take `index` (0 is the newest; the newest again follows new runs).
    Q_INVOKABLE void selectTake(int index);
    // A take read back from History has no results until they are asked for: the strip asks for the ones it draws, so a long
    // history costs nothing until it is looked at (#389).
    Q_INVOKABLE void ensureTake(int index);
    // Puts take `index`'s inputs back in the form (a seed left to chance stays so: its number was the engine's).
    Q_INVOKABLE void reuseInputs(int index);
    // Reuses take `index`'s inputs and runs: "Run again" of a take.
    Q_INVOKABLE QString runAgain(int index);
    // Copies a result's file (a file URL: `full` of a result) to `destination`; why not, or empty. An existing file is replaced
    // (the picker asked first).
    Q_INVOKABLE QString saveResult(const QString &fileUrl, const QUrl &destination);
    // The form's first picture input's key, or empty: where "Use as input" puts a result.
    Q_INVOKABLE QString imageKey() const;
    // The picture input's own sample (the block's default, else the workflow's value, when it is a file that exists): "Use sample".
    // It is also the form's value until the person chooses another, so an app opens ready to Run.
    Q_INVOKABLE void useSample(const QString &key);
    // Takes a result's file into the inputs folder as the value of a picture input (as a dropped file is).
    Q_INVOKABLE QString useResultAsInput(const QString &key, const QString &fileUrl);
    // This tab's picture results, newest first, as {path, url, name} (the recent pictures' shape), for an input to take again.
    Q_INVOKABLE QVariantList resultPictures() const;
    // Empties the output area (results, error, Details) back to the empty state (the takes stay in the strip); the files stay on disk and in the results
    // store. Does nothing while a run of this tab waits or goes.
    Q_INVOKABLE void clearResults();
    // This tab shows the graph (the same workflow, as an ordinary tab); showApp() brings the form back.
    Q_INVOKABLE void openInGraph();
    // The other view of this tab (view.toggle-app, the window bar's App | Graph): the graph from the App view, the App view from the graph.
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void showApp();

    // The window's events of every run (RunService::event); the ones of runs this did not start are ignored.
    void onEvent(const QString &run, const QJsonObject &event);

signals:
    // One quiet line, once per tab, when the workflow's `app` block cannot be used (not well formed, or of a later format): the
    // tab is then an ordinary graph and this says why. The window shows it as it shows any load problem (Workflows' notice).
    void notice(const QString &text);
    // showApp() wants the workflow as it is now (edits made in the graph): the window answers with show().
    void documentWanted();
    void appChanged();
    void shownChanged();
    // A split window: the App view (true) or the graph (false) is wanted in the active area; the window's areas do it (Workspace).
    void areaViewRequested(bool app);
    // view.toggle-app in a split window: the active area shows the other view.
    void areaToggleRequested();
    // The store changed a value itself (see the fields' doc): the control shows `valueOf(key)`.
    void valueChanged(const QString &key);
    // A picture taken by the page's paste or drop was refused (`text`) or taken (empty): the picture input `key` shows it.
    void imageNotice(const QString &key, const QString &text);
    void modelsChanged();
    void runChanged();
    void progressChanged();
    void liveChanged();
    void runsOnChanged();

private:
    // One run of the tab: what it was given and what it made (the shown results are the newest take's, kept as they come).
    struct Take {
        QString run;
        QString status;
        QVariantList results;
        QMap<QString, QJsonValue> values;
        QMap<QString, QString> seedModes;
        QString line;
        bool persisted = false; // read back from the run folders: its results come by replay
        bool requested = false; // that replay was asked for
    };
    struct TabState {
        bool seen = false;
        bool graph = false;
        QMap<QString, QJsonValue> values;
        QMap<QString, QString> seedModes;
        QList<app::AppOutput> outputs; // what the tab's app names as its results
        QString run; // waiting or active
        QString status = QStringLiteral("idle");
        qreal progress = -1;
        QString error;
        QVariantList details;
        QVariantList results;
        QList<Take> takes; // newest first, at most maxTakes
        QString pinned;    // the run the person picked, or empty to follow the newest
        QString liveFrame;
        QString live = QStringLiteral("off"); // Live (#471): "off", "live" or "repeat"
        bool liveDirty = false;               // an input changed (or the last run ended in Repeat) and a run is owed
        QString storedFor;                    // the file the takes of History were read for
        bool cleared = false; // Clear emptied the stage and the strip
    };
    static constexpr int maxTakes = 30;
    QString inputsLine(const QMap<QString, QJsonValue> &values, const QMap<QString, QString> &modes) const;
    void loadStored(TabState &state);
    void requestTake(Take &take);
    void replayEvent(const QString &tab, const QString &run, const QJsonObject &event);
    // The person changed an input (not the store by itself): Live arms its debounce.
    void inputEdited();
    void liveFire();
    // A seed of the shown tab is Random or Increment: the next run differs from the last.
    bool repeatVaries() const;
    void scheduleLive(const QString &tab);
    void syncTake(TabState &state);
    void buildFields();
    QVariantMap fieldFor(const InputBinding &input) const;
    // The key's input, in this form; null when none.
    const InputBinding *inputOf(const QString &key) const;
    QString kindOfField(const QString &key) const;
    QString imageTarget() const; // the focused picture input, else the form's first; empty for none
    QString storeImages(const QStringList &paths, const QString &key);
    QVariantList samplesOf() const;
    void modelHashed(int ticket, const QVariantMap &handle);
    int waitFor(const QString &key); // a ticket for this tab's field
    QString tabOfRun(const QString &run) const;
    static void addResult(const QList<app::AppOutput> &outputs, QVariantList &list, const QJsonObject &event);
    static void addDetail(TabState &state, const QString &message, const QString &hint);
    void end(const QString &tab, const QString &status);

    Start m_start;
    Cancel m_cancel;
    Replay m_replay;
    QString m_runsRoot;
    QHash<QString, QString> m_replaying; // run -> tab: a take's results being read again
    QTimer m_liveTimer;
    const Registry *m_registry;
    QString m_tab;
    QString m_focusedImage;
    QString m_file;
    QString m_takesKey; // the app of the shown tab, as its takes know it (apps/app_takes.h); empty: session takes only
    Document m_document;
    std::optional<app::App> m_app;
    bool m_split = false;   // the window has several areas (setAreas)
    bool m_areaApp = false; // and the active one shows the App view
    QString m_problem;
    QString m_runsOn;
    QString m_silentHost;
    QString m_offMachine;
    QVariantList m_fields;
    AppModels *m_models = nullptr;
    NeedsModel *m_needs = nullptr;
    // Model fields waiting for a hash or a download, by ticket: the tab and the field that asked, so an answer that comes
    // after a tab switch, or for one of two fields of the same type, lands where it was asked for.
    struct Waiting {
        QString tab;
        QString key;
    };
    QHash<int, Waiting> m_waiting;
    int m_tickets = 0;
    QHash<QString, TabState> m_state;
    QSet<QString> m_told; // the tabs whose unusable block was said
};

} // namespace fugue
