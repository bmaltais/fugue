#pragma once

// The Cache page of the settings panel (spec 10.6, #141): what Fugue keeps under ~/.cache/fugue,
// per kind (results, thumbnails, runs), with a Clear for each and the question that says what
// is lost. The measuring and clearing are the cache manager's (engine/cache_manager.h) and run
// on a thread of their own, so the page never waits on the disk; this only asks and shows.
//
// Nothing is cleared without a yes: ask() puts the question in `question` and answer(true)
// clears. The same manager cleans automatically (startAutomatic): once at start and then while
// idle, never while a run is going (it takes the engine lock and skips when it cannot).

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <atomic>

#include "engine/cache_manager.h"

namespace fugue {

class CacheModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through the settings")
    // One map per kind, in order: kind, title, bytes, size (text), detail (a line: how many,
    // the limit, the oldest), items, protectedItems, canClear.
    Q_PROPERTY(QVariantList kinds READ kinds NOTIFY changed)
    Q_PROPERTY(QString total READ total NOTIFY changed)
    // Something is there to clear in some kind (frozen results and runs going do not count).
    Q_PROPERTY(bool canClearAll READ canClearAll NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged) // measuring or clearing
    // The question waiting for a yes: kind, title, what (what is lost), size; empty when none.
    Q_PROPERTY(QVariantMap question READ question NOTIFY questionChanged)
    // How the last clear ended, a sentence.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)

public:
    // `folders` and `policy` default to the standard folders and the settings in effect.
    explicit CacheModel(QObject *parent = nullptr);
    CacheModel(CacheFolders folders, QObject *parent = nullptr);
    ~CacheModel() override;

    QVariantList kinds() const { return m_kinds; }
    QString total() const { return m_total; }
    bool canClearAll() const { return m_canClearAll; }
    bool busy() const { return m_working > 0; }
    QVariantMap question() const { return m_question; }
    QString message() const { return m_message; }

    // Measures again, off the UI thread.
    Q_INVOKABLE void refresh();
    // Asks what clearing `kind` ("results", "thumbnails", "runs", or "all" for every kind) would lose; nothing is cleared yet.
    Q_INVOKABLE void ask(const QString &kind);
    // The person's answer to the question.
    Q_INVOKABLE void answer(bool yes);

    // Cleans once soon after start and then every `intervalMs` while no run is going.
    void startAutomatic(int firstMs = 15000, int intervalMs = 30 * 60 * 1000);
    // Waits for the work that runs now (tests).
    void waitIdle();

signals:
    void changed();
    void busyChanged();
    void questionChanged();
    void messageChanged();
    // A clear or cleaning ended: bytes freed.
    void cleared(const QString &kind, qint64 bytes);

private:
    template <class Work, class Done>
    void work(Work work, Done done);
    void cleanAutomatically();
    void setMessage(const QString &text);

    CacheFolders m_folders;
    QVariantList m_kinds;
    QString m_total;
    bool m_canClearAll = false;
    QVariantMap m_question;
    QString m_message;
    std::atomic<int> m_working{0};
    QList<QThread *> m_threads;
    QTimer m_timer;
    int m_intervalMs = 0;
};

} // namespace fugue
