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
// block lists them; run history and the results store are the existing ones (a later issue shows past takes).
//
// Words: nothing here names a node or a graph port. An output is called by its label, else "Result". Errors are shown as
// the engine words them; `error` is the first line, `details` all of them (with their hint).
//
// Fields (`fields`): one map per input in the form's order: {key, label, kind, value, min?, max?, choices?, section, advanced}
// and what its kind adds. The kind is the control (#386, AppInput.qml draws one per kind):
//   "text"    a prompt or a plain text; `multiline` for the prompt roles
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
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>
#include <optional>

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
    Q_PROPERTY(bool shown READ shown NOTIFY appChanged)
    Q_PROPERTY(QString title READ title NOTIFY appChanged)
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

public:
    using Start = std::function<QString(const RunRequest &)>;
    using Cancel = std::function<void(const QString &run)>;

    // `registry` prunes the block of what the workflow no longer has and checks it; none: the block is used as it is.
    AppView(Start start, Cancel cancel, const Registry *registry = nullptr, QObject *parent = nullptr);

    // The tab the window shows and its workflow (`file`: where it came from, or empty). Called when the tab changes and
    // when the person comes back to the view, so edits made in the graph are seen. A tab seen for the first time opens in
    // its block's defaultView.
    void show(const QString &tab, const Document &document, const QString &file);
    // The tab was closed: what it kept goes (its run is cancelled).
    void forget(const QString &tab);

    bool isApp() const { return m_app.has_value(); }
    bool shown() const { return m_app && !m_state[m_tab].graph; }
    QString title() const { return m_app ? m_app->title : QString(); }
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

    // Queues a run of the shown tab's workflow with the form's values in. Returns its id, empty when it cannot run
    // (`error` says why).
    Q_INVOKABLE QString run();
    Q_INVOKABLE void cancel();
    // This tab shows the graph (the same workflow, as an ordinary tab); showApp() brings the form back.
    Q_INVOKABLE void openInGraph();
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
    // The store changed a value itself (see the fields' doc): the control shows `valueOf(key)`.
    void valueChanged(const QString &key);
    void modelsChanged();
    void runChanged();
    void progressChanged();

private:
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
    };
    void buildFields();
    QVariantMap fieldFor(const InputBinding &input) const;
    // The key's input, in this form; null when none.
    const InputBinding *inputOf(const QString &key) const;
    QString kindOfField(const QString &key) const;
    QString storeImages(const QStringList &paths, const QString &key);
    QVariantList samplesOf() const;
    void modelHashed(int ticket, const QVariantMap &handle);
    int waitFor(const QString &key); // a ticket for this tab's field
    QString tabOfRun(const QString &run) const;
    void addResult(TabState &state, const QJsonObject &event);
    static void addDetail(TabState &state, const QString &message, const QString &hint);
    void end(const QString &tab, const QString &status);

    Start m_start;
    Cancel m_cancel;
    const Registry *m_registry;
    QString m_tab;
    QString m_file;
    Document m_document;
    std::optional<app::App> m_app;
    QString m_problem;
    QVariantList m_fields;
    AppModels *m_models = nullptr;
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
