#include "engine/live_link.h"

#include <algorithm>

namespace fugue {

qint64 LiveLink::quiet() const {
    return std::clamp(std::max(m_base, m_lastRun / 2), m_base, std::max(m_base, ceiling));
}

void LiveLink::setEnabled(bool on, qint64 now) {
    if (on == m_enabled)
        return;
    m_enabled = on;
    m_shownKey.clear();
    m_changedAt = now;
    m_lastRun = 0;
}

void LiveLink::note(const QString &key, bool ready, qint64 now) {
    if (key != m_key) {
        m_key = key;
        m_changedAt = now;
    }
    m_ready = ready;
}

void LiveLink::adopt(const QString &key) {
    m_key = key;
    m_shownKey = key;
}

LiveLink::Step LiveLink::poll(qint64 now) {
    if (m_running) {
        const bool stale = !m_enabled || !m_ready || m_runKey != m_key;
        if (stale && !m_cancelling) {
            m_cancelling = true;
            return {Do::Cancel, -1};
        }
        return {};
    }
    if (!m_enabled || !m_ready || m_key.isEmpty() || m_key == m_shownKey)
        return {};
    const qint64 left = m_changedAt + quiet() - now;
    if (left > 0)
        return {Do::Nothing, left};
    m_running = true;
    m_cancelling = false;
    m_runKey = m_key;
    m_startedAt = now;
    return {Do::Start, -1};
}

void LiveLink::ended(End how, qint64 now) {
    if (!m_running)
        return;
    m_running = false;
    m_cancelling = false;
    if (how == End::Ok) {
        m_shownKey = m_runKey;
        m_lastRun = now - m_startedAt;
    } else if (how == End::Failed) {
        m_enabled = false;
    }
    m_runKey.clear();
}

} // namespace fugue
