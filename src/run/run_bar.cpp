#include "run/run_bar.h"

#include <algorithm>
#include <cmath>

#include <QJsonObject>

#include "engine/json.h"
#include "panels/chrome_visibility.h"
#include "run/canvas_runs.h"

namespace fugue {

namespace {

QPointF clampedPlace(QPointF p) {
    // A bad number (NaN reads as 0.5) is the middle, anything else is held to 0..1.
    const auto unit = [](double v) { return std::isnan(v) ? 0.5 : std::clamp(v, 0.0, 1.0); };
    return {unit(p.x()), unit(p.y())};
}

} // namespace

RunBar::RunBar(CanvasRuns *runs, QString path, ChromeVisibility *chrome)
    : m_runs(runs), m_path(std::move(path)), m_chrome(chrome ? chrome : new ChromeVisibility({}, this)) {
    m_chrome->add(kChromeId);
    connect(m_chrome, &ChromeVisibility::visibilityChanged, this, [this](const QString &id) {
        if (id == QLatin1String(kChromeId))
            emit visibleChanged();
    });
    if (const Result<QJsonObject> read = readJsonObject(m_path)) {
        if (read->contains("visible"))
            m_chrome->seed(kChromeId, read->value("visible").toBool(true));
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

bool RunBar::visible() const { return m_chrome->isVisible(kChromeId); }

void RunBar::setVisible(bool visible) { m_chrome->setVisible(kChromeId, visible); }

void RunBar::setPlace(const QPointF &place) {
    const QPointF wanted = clampedPlace(place);
    if (wanted == m_place)
        return;
    m_place = wanted;
    save();
    emit placeChanged();
}

void RunBar::save() const {
    if (m_path.isEmpty())
        return;
    writeJsonFile(m_path, QJsonObject{{"place", QJsonObject{{"x", m_place.x()}, {"y", m_place.y()}}}, {"batch", m_runs ? m_runs->batch() : 1}});
}

} // namespace fugue
