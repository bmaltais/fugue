#include "run/run_bar.h"

#include <algorithm>
#include <cmath>

#include <QJsonObject>

#include "commands/command_registry.h"
#include "engine/json.h"
#include "run/canvas_runs.h"

namespace fugue {

namespace {

QPointF clampedPlace(QPointF p) {
    // A bad number (NaN reads as 0.5) is the middle, anything else is held to 0..1.
    const auto unit = [](double v) { return std::isnan(v) ? 0.5 : std::clamp(v, 0.0, 1.0); };
    return {unit(p.x()), unit(p.y())};
}

} // namespace

RunBar::RunBar(CanvasRuns *runs, QString path, QObject *parent) : QObject(parent), m_runs(runs), m_path(std::move(path)) {
    if (const Result<QJsonObject> read = readJsonObject(m_path)) {
        m_visible = read->value("visible").toBool(true);
        if (const QJsonObject place = read->value("place").toObject(); !place.isEmpty())
            m_place = clampedPlace({place.value("x").toDouble(0.5), place.value("y").toDouble(0)});
        else if (read->value("dock").toString() == QLatin1String("bottom"))
            m_place = {0.5, 1}; // a file from before the bar moved freely
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

void RunBar::setPlace(const QPointF &place) {
    const QPointF wanted = clampedPlace(place);
    if (wanted == m_place)
        return;
    m_place = wanted;
    save();
    emit placeChanged();
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
    writeJsonFile(m_path, QJsonObject{{"visible", m_visible}, {"place", QJsonObject{{"x", m_place.x()}, {"y", m_place.y()}}}, {"batch", m_runs ? m_runs->batch() : 1}});
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
