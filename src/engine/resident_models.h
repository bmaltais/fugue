#pragma once

// The resident models of a process (spec 9.3): which models are loaded right
// now, in memory, least recently used first. It is separate from the result
// cache and holds no results.
//
// The scheduler reads it to prefer ready nodes that reuse a loaded model, and
// on out_of_memory evicts the least recently used model, never everything.
// A kernel that loads a model keeps it here under resourceIdentity(handle),
// with the loaded object as the payload; dropping the payload is what unloads
// it. For a provider node the scheduler itself records the model the
// provider's server now holds ("<instance name>\n<model identity>", no
// payload), since each model server holds one model at a time.
//
// Thread-safe: kernels call it from worker threads.

#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <memory>

namespace fugue {

class ResidentModels {
public:
    // Makes `key` resident and the most recently used. A payload already
    // held under the key is replaced.
    void put(const QString &key, std::shared_ptr<void> payload = nullptr);
    // The payload, which also counts as a use; null when the key is not
    // resident or has no payload.
    std::shared_ptr<void> get(const QString &key);
    bool contains(const QString &key) const;
    // Counts as a use, when the key is resident.
    void touch(const QString &key);

    // Drops the least recently used model that is not in `keep`, and gives
    // its key; an empty string when there was none to drop.
    QString evictLeastRecentlyUsed(const QStringList &keep = {});
    void remove(const QString &key);
    void clear();

    // Least recently used first.
    QStringList keys() const;

private:
    struct Entry {
        QString key;
        std::shared_ptr<void> payload;
    };

    mutable QMutex m_mutex;
    QList<Entry> m_entries; // least recently used first
};

} // namespace fugue
