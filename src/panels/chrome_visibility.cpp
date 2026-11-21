#include "panels/chrome_visibility.h"

#include <QJsonObject>

#include "engine/json.h"

namespace fugue {

ChromeVisibility::ChromeVisibility(QString path, QObject *parent) : QObject(parent), m_path(std::move(path)) {
    if (m_path.isEmpty())
        return;
    if (const Result<QJsonObject> read = readJsonObject(m_path)) {
        const QJsonObject visible = read->value("visible").toObject();
        for (auto it = visible.begin(); it != visible.end(); ++it) {
            // A bool is the older form; an auto-hide bar writes its mode as a word (#160).
            if (it.value().isBool())
                m_stored.insert(it.key(), it.value().toBool());
            else if (it.value().toString() == QLatin1String("shown"))
                m_stored.insert(it.key(), true);
            else if (it.value().toString() == QLatin1String("auto-hide"))
                m_stored.insert(it.key(), false);
        }
    }
}

void ChromeVisibility::add(const QString &id, bool shownByDefault) {
    if (id.isEmpty() || m_wanted.contains(id))
        return;
    m_order.append(id);
    m_default.insert(id, shownByDefault);
    m_wanted.insert(id, m_stored.value(id, shownByDefault));
}

void ChromeVisibility::setAutoHide(const QString &id) {
    m_autoHide.insert(id);
}

QString ChromeVisibility::mode(const QString &id) const {
    if (!m_wanted.contains(id))
        return {};
    return m_wanted.value(id) ? QStringLiteral("shown") : QStringLiteral("auto-hide");
}

void ChromeVisibility::seed(const QString &id, bool shown) {
    if (m_wanted.contains(id) && !m_stored.contains(id))
        m_wanted[id] = shown;
}

void ChromeVisibility::setExclusive(const QStringList &ids) {
    QStringList known;
    for (const QString &id : ids)
        if (m_wanted.contains(id))
            known.append(id);
    if (known.size() < 2)
        return;
    m_groups.append(known);
    bool taken = false;
    for (const QString &id : known) {
        if (!m_wanted.value(id))
            continue;
        if (taken)
            m_wanted[id] = false;
        taken = true;
    }
}

bool ChromeVisibility::isVisible(const QString &id) const {
    return m_wanted.value(id, false) && (!m_focus || m_asked.contains(id));
}

QVariantMap ChromeVisibility::shown() const {
    QVariantMap map;
    for (const QString &id : m_order)
        map.insert(id, isVisible(id));
    return map;
}

QHash<QString, bool> ChromeVisibility::snapshot() const {
    QHash<QString, bool> now;
    for (const QString &id : m_order)
        now.insert(id, isVisible(id));
    return now;
}

template <typename Edit> void ChromeVisibility::apply(Edit edit, bool saves) {
    const QHash<QString, bool> before = snapshot();
    edit();
    bool any = false;
    for (const QString &id : m_order) {
        const bool now = isVisible(id);
        if (now == before.value(id))
            continue;
        any = true;
        emit visibilityChanged(id, now);
    }
    if (saves)
        save();
    if (any)
        emit changed();
}

void ChromeVisibility::setVisible(const QString &id, bool visible) {
    if (!m_wanted.contains(id) || isVisible(id) == visible)
        return;
    apply(
        [&] {
            m_wanted[id] = visible;
            m_stored[id] = visible;
            if (visible)
                m_asked.insert(id);
            else
                m_asked.remove(id);
            if (!visible)
                return;
            for (const QStringList &group : m_groups) {
                if (!group.contains(id))
                    continue;
                for (const QString &other : group)
                    if (other != id && m_wanted.value(other)) {
                        m_wanted[other] = false;
                        m_stored[other] = false;
                        m_asked.remove(other);
                    }
            }
        },
        true);
    if (!visible)
        emit hiddenByRequest(id);
}

void ChromeVisibility::setFocusMode(bool on) {
    if (on == m_focus)
        return;
    apply(
        [&] {
            m_focus = on;
            m_asked.clear();
        },
        false);
    emit focusModeChanged();
}

void ChromeVisibility::reset() {
    const bool wasFocused = m_focus;
    apply(
        [&] {
            m_focus = false;
            m_asked.clear();
            for (const QString &id : m_order) {
                m_wanted[id] = m_default.value(id);
                m_stored[id] = m_default.value(id);
            }
        },
        true);
    if (wasFocused)
        emit focusModeChanged();
}

void ChromeVisibility::save() const {
    if (m_path.isEmpty())
        return;
    QJsonObject visible;
    for (auto it = m_stored.begin(); it != m_stored.end(); ++it) {
        if (m_autoHide.contains(it.key()))
            visible.insert(it.key(), it.value() ? QStringLiteral("shown") : QStringLiteral("auto-hide"));
        else
            visible.insert(it.key(), it.value());
    }
    writeJsonFile(m_path, QJsonObject{{"visible", visible}});
}

} // namespace fugue
