#pragma once

// The Packs page of the settings panel (spec 6.6-6.8, #25): the installed packs and what the
// load said about each, and install, update, remove and trust, with the same questions the CLI
// asks. The work (git, copying, a pack's setup) runs on a thread of its own; every question
// reaches the person through `question` and waits for answer(): nothing is installed, trusted,
// removed or run on its own. QML only asks and answers; the engine (pack_manager.h) decides.
//
// A pack installed, updated, removed or trusted here is usable at once: packsModified() has the
// window read its node types again (#70, engine/registry_reloader.h).

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
    // (this window has loaded it), secrets (one map per declared secret: name, label, description,
    // set; never the value).
    Q_PROPERTY(QVariantList packs READ packs NOTIFY packsChanged)
    // The library's groups (#74): one map each, id (local/<name>), name, version.
    Q_PROPERTY(QVariantList library READ library NOTIFY packsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // The question waiting for a person, empty when none: kind (install, update, remove,
    // trust, setup), id, name, version, oldVersion, source, runtime, permissions, secrets (names), nodes,
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

public:
    // `registry` is the window's (to tell which packs it has loaded); null is fine. The two
    // paths default to the standard ones.
    explicit PacksModel(const Registry *registry = nullptr, const QString &packsFolder = {}, const QString &trustFile = {},
                        QObject *parent = nullptr);
    ~PacksModel() override;

    void setRegistry(const Registry *registry) { m_registry = registry; refresh(); }

    QVariantList packs() const { return m_packs; }
    QVariantList library() const { return m_library; }
    bool busy() const { return m_busy; }
    QVariantMap question() const { return m_question; }
    QStringList log() const { return m_log; }
    QString message() const { return m_message; }
    QString failure() const { return m_failure; }
    bool questionReady() const { return m_questionReady; }

    // Read the packs folders again.
    Q_INVOKABLE void refresh();
    // A folder, or a git address with the tag or commit it is pinned to.
    Q_INVOKABLE void add(const QString &location, const QString &ref);
    // One pack, or every pack with a recorded source when `id` is empty.
    Q_INVOKABLE void update(const QString &id);
    Q_INVOKABLE void remove(const QString &id);
    Q_INVOKABLE void trust(const QString &id);
    // A secret a pack declares (6.4, #402): stored in pack-secrets.json (0600), or dropped. Quick, so they run on the
    // window's thread; true when done, and `failure` says why not. The value is never read back into the page:
    // the packs list says only whether each name is set and the origin it is for. A name the pack does not declare is
    // refused, and so are a value the command line would refuse (more than one line, a NUL, over 4096 bytes) and a
    // missing origin for a secret whose pack requires one (6.4.1).
    Q_INVOKABLE bool setSecret(const QString &packId, const QString &name, const QString &value, const QString &origin = {});
    Q_INVOKABLE bool clearSecret(const QString &packId, const QString &name);
    // A library group renamed (its folder and id follow) or removed (the page asks first, on
    // itself: these are quick and run on the window's thread, not through `question`; workflows
    // keep their own copy). The one call that deletes a library group: the page and the node
    // library's trash can (#156, Main.qml) both use it. Refused while a pack action is under way. True when done; `failure`
    // says why not, set when this returns.
    Q_INVOKABLE bool renameLibraryGroup(const QString &id, const QString &name);
    Q_INVOKABLE bool removeLibraryGroup(const QString &id);
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
    // An action changed what is installed or trusted (it is over, and refresh() has run).
    void packsModified();
    // A library group was renamed here (#74): the workflow shown, when linked to it, follows.
    void libraryGroupRenamed(const QString &from, const QString &to);

private:
    template <typename Work> void start(Work work);
    PackPrompts prompts();
    Answer ask(const QString &kind, const PackSummary &pack);
    void finish(const QString &message, const Errors &errors, bool changed);
    QString libraryFolder() const;
    void reportLibrary(const QString &message, const Errors &errors);

    const Registry *m_registry;
    QString m_packsFolder;
    QString m_trustFile;
    QVariantList m_packs;
    QVariantList m_library;
    QVariantMap m_question;
    QStringList m_log;
    QString m_message;
    QString m_failure;
    bool m_busy = false;
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
