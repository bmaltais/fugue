#include "panels/side_panels.h"

#include <QJsonObject>
#include <algorithm>

#include "commands/command_registry.h"
#include "engine/json.h"
#include "panels/chrome_visibility.h"

namespace fugue {

namespace {
QString knownTab(const QString &tab) {
    return tab == QLatin1String("nodes") || tab == QLatin1String("workflows") ? tab : QString();
}
} // namespace

namespace {
const QString nodesId = QStringLiteral("node-library");
const QString workflowsId = QStringLiteral("workflows-panel");
} // namespace

SidePanels::SidePanels(QString path, ChromeVisibility *chrome)
    : m_path(std::move(path)), m_chrome(chrome ? chrome : new ChromeVisibility({}, this)) {
    m_chrome->add(nodesId, false);
    m_chrome->add(workflowsId, false);
    m_chrome->setExclusive({nodesId, workflowsId});
    if (const Result<QJsonObject> read = readJsonObject(m_path)) {
        if (const QString old = knownTab(read->value("tab").toString()); !old.isEmpty()) {
            m_chrome->seed(old == QLatin1String("nodes") ? nodesId : workflowsId, true);
            m_chrome->setExclusive({nodesId, workflowsId});
        }
        m_dock = read->value("dock").toString() == QLatin1String("right") ? QStringLiteral("right") : QStringLiteral("left");
        m_width = std::clamp(read->value("width").toInt(kDefaultWidth), kMinWidth, kMaxWidth);
    }
    m_shownTab = this->tab();
    connect(m_chrome, &ChromeVisibility::changed, this, [this] {
        const QString now = this->tab();
        if (now == m_shownTab)
            return;
        m_shownTab = now;
        emit tabChanged();
    });
}

QString SidePanels::tab() const {
    return m_chrome->isVisible(nodesId) ? QStringLiteral("nodes") : m_chrome->isVisible(workflowsId) ? QStringLiteral("workflows") : QString();
}

void SidePanels::setTab(const QString &tab) {
    const QString wanted = knownTab(tab);
    if (wanted.isEmpty()) {
        m_chrome->setVisible(nodesId, false);
        m_chrome->setVisible(workflowsId, false);
    } else {
        m_chrome->setVisible(wanted == QLatin1String("nodes") ? nodesId : workflowsId, true);
    }
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
    setTab(this->tab() == tab ? QString() : tab);
    if (!this->tab().isEmpty())
        emit focusRequested();
}

void SidePanels::save() const {
    if (m_path.isEmpty())
        return;
    writeJsonFile(m_path, QJsonObject{{"dock", m_dock}, {"width", m_width}});
}

void SidePanels::registerCommands(CommandRegistry &commands) {
    for (const char *name : {"nodes", "workflows"}) {
        const QString tab = QString::fromLatin1(name);
        commands.setHandler(QStringLiteral("panel.") + tab, [this, tab] { toggle(tab); });
        commands.setCheckedWhen(QStringLiteral("panel.") + tab, [this, tab] { return this->tab() == tab; });
    }
}

} // namespace fugue
