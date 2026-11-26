#pragma once

// The window's "Get ready" sheet (spec 6.6, 8.9, 11.10; #379): what the shown workflow needs (the packs to install and
// trust, their environments and the missing models) as one list, asked once, then run with a row of progress each. The
// plan and the executor are engine/readiness.h's, the same the CLI's `prepare` commands use: this class only shows the
// plan to QML (`view`), runs the executor on a thread of its own after the person's one yes (start()), and keeps the
// choice of where models go (Models go into / Change…, spec 8.9).
//
// QML only shows and answers. Nothing is installed, trusted, set up or downloaded unless start() is called, which the
// sheet's "Install and trust all" button does.

#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <atomic>
#include <functional>
#include <memory>

#include "engine/model_library.h"
#include "engine/readiness.h"

namespace fugue {

class Registry;

class ReadinessSheet : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made by Workflows")
    Q_PROPERTY(bool open READ isOpen NOTIFY changed)
    // "review" (the list, waiting for the yes), "running", or "finished" (ended: all done, or stopped; after all done the sheet closes itself).
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    // What the sheet shows: headline, packs and models (one map each: name, detail lines, state and detail text), the
    // destination ("Models go into": destination, destinationTitle, destinations [{path, title}], freeText, sizeText,
    // lowSpace), problems (lines), and once ended: ok, message, retry.
    Q_PROPERTY(QVariantMap view READ view NOTIFY changed)

public:
    using Request = std::function<ReadinessRequest()>;
    // `trustPath` is the trust file the window uses; `request` says what the shown workflow needs, read each time.
    ReadinessSheet(const Registry *registry, QString trustPath, Request request, QObject *parent = nullptr);
    ~ReadinessSheet() override;

    bool isOpen() const { return m_open; }
    QString phase() const { return m_phase; }
    QVariantMap view() const { return m_view; }

    // The plan of what the shown workflow needs now, built without opening the sheet or changing it (the Needs panel, #469).
    ReadinessPlan currentPlan() const;
    // Builds the list and opens the sheet; with nothing to do it says so.
    Q_INVOKABLE void show();
    // Use what you have (#605): `source` says, for a missing model's hash, the file on disk of the same weights as {text, file, tier, ...}
    // (empty for none); a row with one offers "Use what you have: <file> (same model, <tier>)" beside its download.
    using UseSource = std::function<QVariantMap(const QString &hash)>;
    void setUseSource(UseSource source) { m_useSource = std::move(source); }
    // The person chose the file for the model of this hash (`useRequested`): the window sets it and asks refresh() for the list again.
    Q_INVOKABLE void useOnDisk(const QString &hash);
    // The list again after something outside changed what is missing (a file chosen in place of a model); only while reviewing.
    void refresh();
    // A line in the sheet's message place while it is open for review (the use of a file failed): shown with the rows, not behind them.
    void say(const QString &message);
    // The person's one yes: runs every row, then ends. Ignored unless reviewing.
    Q_INVOKABLE void start();
    // Stops a run (the rows not done stay undone); closes a review.
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void close();
    // Models go into `root`, one of the choices. Only the list changes: the settings are written at the yes, not before.
    Q_INVOKABLE void changeDestination(const QString &root);
    // A folder picked (a path or file URL): offered as a ComfyUI-layout model folder and chosen. At the yes it is added to the
    // model folders and becomes the settings' download_folder; before that nothing is written.
    Q_INVOKABLE void pickDestination(const QString &folder);
    // The person's answer to the question a pack asks mid-run (view.question): Allow or Skip.
    Q_INVOKABLE void answer(bool allow);
    // A model folder as the sheet and Settings > Folders name it: "Fugue's models folder" for `own`, else the path with ~.
    static QString rootTitle(const QString &root, const QString &own);
    // For the tests: waits for a run to end.
    bool waitIdle(int msec = 30000);

signals:
    void changed();
    // A pack was installed or trusted: the window reads its node types again.
    void packsModified();
    // A run ended (ok: every row done).
    void finished(bool ok);
    // The person chose, in the list, the file they have for the model of this hash (#605).
    void useRequested(const QString &hash);
    // The sheet was opened (show()).
    void shown();
    // A run ended with everything ready: the sheet has closed itself.
    void succeeded();

private:
    void rebuild();
    ReadinessPlan buildPlan() const;
    // The model folders as the settings have them, with a folder picked here and the one chosen, not yet written.
    ModelLibrary library() const;
    // Writes the folder choice (a picked model folder, download_folder) at the yes.
    bool savePlacement();
    bool askMore(const PackSummary &pack, const QStringList &permissions);
    void publish();
    // A download's progress: the view is made again at most every kProgressMs (each change rebuilds the sheet's rows in QML).
    void publishSoon();
    void setRow(int row, RowState state, const QString &detail);
    void ended(const ReadinessOutcome &outcome);

    const Registry *m_registry;
    QString m_trustPath;
    Request m_request;
    UseSource m_useSource;
    ReadinessPlan m_plan;
    bool m_open = false;
    QString m_phase = QStringLiteral("review");
    QString m_message;
    QStringList m_retry;
    bool m_ok = false;
    QList<RowState> m_states;
    QStringList m_details;
    QList<double> m_fractions;
    QVariantMap m_view;
    QThread *m_thread = nullptr;
    std::unique_ptr<const Registry> m_runTypes; // the run's copy of the node types (start())
    QTimer m_publishTimer; // publishSoon()'s
    QString m_ownRoot;     // Fugue's own models folder, as rebuild() found it
    QString m_destination; // chosen with Change…, not yet written
    QString m_picked;      // a folder picked with Pick a folder…, not yet a model folder
    QVariantMap m_question;
    QMutex m_mutex;
    QWaitCondition m_asked;
    int m_answer = -1;
    std::atomic<bool> m_cancel{false};
};

} // namespace fugue
