#pragma once

// The window's "Get ready" sheet (spec 6.6, 8.9, 11.10; #379): what the shown workflow needs (the packs to install and
// trust, their environments and the missing models) as one list, asked once, then run with a row of progress each. The
// plan and the executor are engine/readiness.h's, the same the CLI's `prepare` commands use: this class only shows the
// plan to QML (`view`), runs the executor on a thread of its own after the person's one yes (start()), and keeps the
// choice of where models go (Models go into / Change…, spec 8.9).
//
// QML only shows and answers. Nothing is installed, trusted, set up or downloaded unless start() is called, which the
// sheet's "Install and trust all" button does.

#include <QObject>
#include <QString>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <atomic>
#include <functional>

#include "engine/readiness.h"

namespace fugue {

class Registry;

class ReadinessSheet : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made by Workflows")
    Q_PROPERTY(bool open READ isOpen NOTIFY changed)
    // "review" (the list, waiting for the yes), "running", or "finished" (ended: all done, or stopped).
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
    // True when there is something to get ready for the shown workflow (a plan with rows), read now.
    bool needed() const;

    // Builds the list and opens the sheet; with nothing to do it says so.
    Q_INVOKABLE void show();
    // The person's one yes: runs every row, then ends. Ignored unless reviewing.
    Q_INVOKABLE void start();
    // Stops a run (the rows not done stay undone); closes a review.
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void close();
    // Models go into `root`, one of the choices: remembered as the settings' download_folder.
    Q_INVOKABLE void changeDestination(const QString &root);
    // A folder picked (a path or file URL): added to the model folders as a ComfyUI-layout root, then used.
    Q_INVOKABLE void pickDestination(const QString &folder);
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

private:
    void rebuild();
    void publish();
    void setRow(int row, RowState state, const QString &detail);
    void ended(const ReadinessOutcome &outcome);

    const Registry *m_registry;
    QString m_trustPath;
    Request m_request;
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
    std::atomic<bool> m_cancel{false};
};

} // namespace fugue
