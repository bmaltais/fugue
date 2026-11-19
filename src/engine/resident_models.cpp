#include "resident_models.h"

#include <QMutexLocker>

namespace fugue {

// Payloads are released after the lock is given up: unloading a model can be
// slow, and its destructor may call back in here.

void ResidentModels::put(const QString &key, std::shared_ptr<void> payload) {
    std::shared_ptr<void> old;
    QMutexLocker lock(&m_mutex);
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).key == key) {
            old = m_entries.takeAt(i).payload;
            break;
        }
    }
    m_entries.append(Entry{key, std::move(payload)});
    lock.unlock();
}

std::shared_ptr<void> ResidentModels::get(const QString &key) {
    QMutexLocker lock(&m_mutex);
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).key == key) {
            m_entries.move(i, m_entries.size() - 1);
            return m_entries.last().payload;
        }
    }
    return nullptr;
}

bool ResidentModels::contains(const QString &key) const {
    QMutexLocker lock(&m_mutex);
    for (const Entry &entry : m_entries)
        if (entry.key == key)
            return true;
    return false;
}

void ResidentModels::touch(const QString &key) { get(key); }

QString ResidentModels::evictLeastRecentlyUsed(const QStringList &keep) {
    Entry evicted;
    QMutexLocker lock(&m_mutex);
    for (int i = 0; i < m_entries.size(); ++i) {
        if (!keep.contains(m_entries.at(i).key)) {
            evicted = m_entries.takeAt(i);
            break;
        }
    }
    lock.unlock();
    return evicted.key;
}

void ResidentModels::remove(const QString &key) {
    Entry removed;
    QMutexLocker lock(&m_mutex);
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).key == key) {
            removed = m_entries.takeAt(i);
            break;
        }
    }
    lock.unlock();
}

void ResidentModels::clear() {
    QList<Entry> all;
    QMutexLocker lock(&m_mutex);
    all.swap(m_entries);
    lock.unlock();
}

QStringList ResidentModels::keys() const {
    QMutexLocker lock(&m_mutex);
    QStringList keys;
    for (const Entry &entry : m_entries)
        keys.append(entry.key);
    return keys;
}

} // namespace fugue
