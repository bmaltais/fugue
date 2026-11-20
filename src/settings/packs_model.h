#pragma once

// The Packs page of the settings panel (spec 6.6-6.8, #25): the installed packs and what the
// load said about each, and install, update, remove and trust, with the same questions the CLI
// asks. The work (git, copying, a pack's setup) runs on a thread of its own; every question
// reaches the person through `question` and waits for answer(): nothing is installed, trusted,
// removed or run on its own. QML only asks and answers; the engine (pack_manager.h) decides.
//
// The running window keeps the node types it started with, so a change to the packs shows as
// `restartNeeded` until Fugue is started again.

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QWaitCondition>
#include <QtQmlIntegration/qqmlintegration.h>

#include <atomic>

#include "engine/pack_manager.h"

namespace fugue {

class Registry;

class PacksModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through the settings")
    // One map per pack found, in search order: id, version, name, path, enabled, trusted,
    // removable (it is in the packs folder), nodes (count), source, problems (lines), active
    // (this window has loaded it).
    Q_PROPERTY(QVariantList packs READ packs NOTIFY packsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // The question waiting for a person, empty when none: kind (install, update, remove,
    // trust, setup), id, name, version, oldVersion, source, runtime, permissions, nodes,
    // command and description (setup), folder.
    Q_PROPERTY(QVariantMap question READ question NOTIFY questionChanged)
    // What git and a setup printed, a line each, while the work runs.
    Q_PROPERTY(QStringList log READ log NOTIFY logChanged)
    // How the last action ended: a sentence, and the reason when it failed.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(QString failure READ failure NOTIFY messageChanged)
    // False for a moment after a question appears, so a click or key meant for the one before
    // cannot answer the next (install is followed at once by the setup question).
    Q_PROPERTY(bool questionReady READ questionReady NOTIFY questionChanged)
    Q_PROPERTY(bool restartNeeded READ restartNeeded NOTIFY packsChanged)

public:
    // `registry` is the window's (to tell which packs it has loaded); null is fine. The two
    // paths default to the standard ones.
    explicit PacksModel(const Registry *registry = nullptr, const QString &packsFolder = {}, const QString &trustFile = {},
                        QObject *parent = nullptr);
    ~PacksModel() override;

    void setRegistry(const Registry *registry) { m_registry = registry; refresh(); }

    QVariantList packs() const { return m_packs; }
    bool busy() const { return m_busy; }
    QVariantMap question() const { return m_question; }
    QStringList log() const { return m_log; }
    QString message() const { return m_message; }
    QString failure() const { return m_failure; }
    bool restartNeeded() const { return m_restartNeeded; }
    bool questionReady() const { return m_questionReady; }

    // Read the packs folders again.
    Q_INVOKABLE void refresh();
    // A folder, or a git address with the tag or commit it is pinned to.
    Q_INVOKABLE void add(const QString &location, const QString &ref);
    // One pack, or every pack with a recorded source when `id` is empty.
    Q_INVOKABLE void update(const QString &id);
    Q_INVOKABLE void remove(const QString &id);
    Q_INVOKABLE void trust(const QString &id);
    // The person's answer to `question`; ignored until questionReady.
    Q_INVOKABLE void answer(bool yes);
    // Stops the work: a question is answered no, git and a setup are killed.
    Q_INVOKABLE void cancel();
    // Only for the tests: waits for the work to end.
    bool waitIdle(int msec = 30000);

signals:
    void packsChanged();
    void busyChanged();
    void questionChanged();
    void logChanged();
    void messageChanged();

private:
    template <typename Work> void start(Work work);
    PackPrompts prompts();
    Answer ask(const QString &kind, const PackSummary &pack);
    void finish(const QString &message, const Errors &errors, bool changed);

    const Registry *m_registry;
    QString m_packsFolder;
    QString m_trustFile;
    QVariantList m_packs;
    QVariantMap m_question;
    QStringList m_log;
    QString m_message;
    QString m_failure;
    bool m_busy = false;
    bool m_restartNeeded = false;
    bool m_questionReady = false;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_sawNo{false}; // the person said no to something in this action

    QMutex m_mutex;
    QWaitCondition m_answered;
    int m_answer = -1; // -1 waiting, else an Answer
    bool m_closing = false;
    QThread *m_thread = nullptr;
};

} // namespace fugue
