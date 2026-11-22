#include "panels/side_panels.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <algorithm>

#include "commands/command_registry.h"
#include "engine/json.h"
#include "panels/chrome_visibility.h"

namespace fugue {

namespace {

// The registered tabs, in the strip's order. A `panel` row is a docked panel: add it here, give its
// command a handler (registerCommands does it for every panel row), and the strip shows its tab.
// An `action` row (#192) is a tab that launches a command someone else owns (the Templates dialog):
// it has no panel, no registry id, no handler and no checked state here, and the strip draws it
// below the panel tabs.
enum class Kind { Panel, Action };
struct Panel {
    Kind kind;
    const char *tab;
    const char *chromeId;
    const char *command;
    const char *glyph; // Lucide name (share/fonts/lucide-fugue.txt)
    const char *name;
};
constexpr Panel panelTable[] = {
    {Kind::Panel, "nodes", "node-library", "panel.nodes", "boxes", QT_TRANSLATE_NOOP("Panel", "Node library")},
    {Kind::Panel, "workflows", "workflows-panel", "panel.workflows", "workflow", QT_TRANSLATE_NOOP("Panel", "Workflows")},
    {Kind::Action, "templates", "", "file.templates", "layout-template", QT_TRANSLATE_NOOP("Panel", "Templates")},
};

// The panel row for `tab`; an action row is not one.
const Panel *panelFor(const QString &tab) {
    for (const Panel &panel : panelTable)
        if (panel.kind == Kind::Panel && tab == QLatin1String(panel.tab))
            return &panel;
    return nullptr;
}

QString knownTab(const QString &tab) { return panelFor(tab) ? tab : QString(); }

const QString stripId = QStringLiteral("side-tabs");

} // namespace

SidePanels::SidePanels(QString path, ChromeVisibility *chrome)
    : m_path(std::move(path)), m_chrome(chrome ? chrome : new ChromeVisibility({}, this)) {
    QStringList ids;
    for (const Panel &panel : panelTable) {
        if (panel.kind != Kind::Panel)
            continue;
        m_chrome->add(QString::fromLatin1(panel.chromeId), false);
        ids << QString::fromLatin1(panel.chromeId);
    }
    m_chrome->setExclusive(ids);
    m_chrome->add(stripId, true);
    if (const Result<QJsonObject> read = readJsonObject(m_path)) {
        if (const QString old = knownTab(read->value("tab").toString()); !old.isEmpty()) {
            m_chrome->seed(QString::fromLatin1(panelFor(old)->chromeId), true);
            m_chrome->setExclusive(ids);
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
    for (const Panel &panel : panelTable)
        if (panel.kind == Kind::Panel && m_chrome->isVisible(QString::fromLatin1(panel.chromeId)))
            return QString::fromLatin1(panel.tab);
    return {};
}

QVariantList SidePanels::panels() const {
    QVariantList list;
    const QString open = tab();
    for (const Panel &panel : panelTable) {
        const QString command = QString::fromLatin1(panel.command);
        list << QVariantMap{
            {"tab", QString::fromLatin1(panel.tab)},
            {"kind", QString::fromLatin1(panel.kind == Kind::Action ? "action" : "panel")},
            {"name", QCoreApplication::translate("Panel", panel.name)},
            {"glyph", QString::fromLatin1(panel.glyph)},
            {"command", command},
            {"keys", m_commands ? m_commands->entry(command).value("keys") : QVariant(QStringList())},
            {"open", panel.kind == Kind::Panel && open == QLatin1String(panel.tab)},
        };
    }
    return list;
}

void SidePanels::setTab(const QString &tab) {
    if (const Panel *wanted = panelFor(tab)) {
        m_chrome->setVisible(QString::fromLatin1(wanted->chromeId), true); // the group hides the others, in one change
        return;
    }
    for (const Panel &panel : panelTable)
        if (panel.kind == Kind::Panel)
            m_chrome->setVisible(QString::fromLatin1(panel.chromeId), false);
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
    m_commands = &commands;
    for (const Panel &panel : panelTable) {
        if (panel.kind != Kind::Panel)
            continue;
        const QString tab = QString::fromLatin1(panel.tab);
        const QString command = QString::fromLatin1(panel.command);
        commands.setHandler(command, [this, tab] { toggle(tab); });
        commands.setCheckedWhen(command, [this, tab] { return this->tab() == tab; });
    }
}

} // namespace fugue
