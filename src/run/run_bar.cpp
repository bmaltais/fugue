#include "run/run_bar.h"

#include <QJsonObject>

#include "commands/command_registry.h"
#include "engine/json.h"
#include "run/canvas_runs.h"

namespace fugue {

RunBar::RunBar(CanvasRuns *runs, QString path, QObject *parent) : QObject(parent), m_runs(runs), m_path(std::move(path)) {
    if (const Result<QJsonObject> read = readJsonObject(m_path)) {
        m_visible = read->value("visible").toBool(true);
        m_dock = read->value("dock").toString() == QLatin1String("bottom") ? QStringLiteral("bottom") : QStringLiteral("top");
        if (m_runs)
            m_runs->setBatch(read->value("batch").toInt(1));
    }
    if (m_runs)
        connect(m_runs, &CanvasRuns::batchChanged, this, &RunBar::save);
}

void RunBar::setVisible(bool visible) {
    if (visible == m_visible)
        return;
    m_visible = visible;
    save();
    emit visibleChanged();
}

void RunBar::setDock(const QString &dock) {
    const QString wanted = dock == QLatin1String("bottom") ? QStringLiteral("bottom") : QStringLiteral("top");
    if (wanted == m_dock)
        return;
    m_dock = wanted;
    save();
    emit dockChanged();
}

void RunBar::setQueueOpen(bool open) {
    if (open == m_queueOpen)
        return;
    m_queueOpen = open;
    emit queueOpenChanged();
}

void RunBar::save() const {
    if (m_path.isEmpty())
        return;
    writeJsonFile(m_path, QJsonObject{{"visible", m_visible}, {"dock", m_dock}, {"batch", m_runs ? m_runs->batch() : 1}});
}

void RunBar::registerCommands(CommandRegistry &commands) {
    commands.setHandler("view.toggle-run-toolbar", [this] { setVisible(!m_visible); });
    commands.setCheckedWhen("view.toggle-run-toolbar", [this] { return m_visible; });
    // Opening the panel shows the toolbar it hangs from.
    commands.setHandler("panel.queue", [this] {
        if (!m_queueOpen)
            setVisible(true);
        setQueueOpen(!m_queueOpen);
        if (m_queueOpen)
            emit queueFocusRequested();
    });
    commands.setCheckedWhen("panel.queue", [this] { return m_queueOpen; });
}

} // namespace fugue
