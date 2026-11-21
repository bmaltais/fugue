#include "panels/side_panels.h"

#include <QJsonObject>
#include <algorithm>

#include "commands/command_registry.h"
#include "engine/json.h"

namespace fugue {

namespace {
QString knownTab(const QString &tab) {
    return tab == QLatin1String("nodes") || tab == QLatin1String("workflows") ? tab : QString();
}
} // namespace

SidePanels::SidePanels(QString path, QObject *parent) : QObject(parent), m_path(std::move(path)) {
    if (const Result<QJsonObject> read = readJsonObject(m_path)) {
        m_tab = knownTab(read->value("tab").toString());
        m_dock = read->value("dock").toString() == QLatin1String("right") ? QStringLiteral("right") : QStringLiteral("left");
        m_width = std::clamp(read->value("width").toInt(kDefaultWidth), kMinWidth, kMaxWidth);
    }
}

void SidePanels::setTab(const QString &tab) {
    const QString wanted = knownTab(tab);
    if (wanted == m_tab)
        return;
    m_tab = wanted;
    save();
    emit tabChanged();
}

void SidePanels::setDock(const QString &dock) {
    const QString wanted = dock == QLatin1String("right") ? QStringLiteral("right") : QStringLiteral("left");
    if (wanted == m_dock)
        return;
    m_dock = wanted;
    save();
    emit dockChanged();
}

void SidePanels::setWidth(int width) {
    const int wanted = std::clamp(width, kMinWidth, kMaxWidth);
    if (wanted == m_width)
        return;
    m_width = wanted;
    save();
    emit widthChanged();
}

void SidePanels::toggle(const QString &tab) {
    const QString wanted = m_tab == tab ? QString() : tab;
    setTab(wanted);
    if (!m_tab.isEmpty())
        emit focusRequested();
}

void SidePanels::save() const {
    if (m_path.isEmpty())
        return;
    writeJsonFile(m_path, QJsonObject{{"tab", m_tab}, {"dock", m_dock}, {"width", m_width}});
}

void SidePanels::registerCommands(CommandRegistry &commands) {
    for (const char *name : {"nodes", "workflows"}) {
        const QString tab = QString::fromLatin1(name);
        commands.setHandler(QStringLiteral("panel.") + tab, [this, tab] { toggle(tab); });
        commands.setCheckedWhen(QStringLiteral("panel.") + tab, [this, tab] { return m_tab == tab; });
    }
}

} // namespace fugue
