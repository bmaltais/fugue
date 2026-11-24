#include "canvas/task_bar_place.h"

#include <algorithm>
#include <cmath>

#include <QFile>
#include <QJsonObject>

#include "engine/float_place.h"
#include "engine/json.h"

namespace fugue {

namespace {

QPointF held(QPointF p) {
    const auto unit = [](double v) { return std::isnan(v) ? 0.5 : std::clamp(v, 0.0, 1.0); };
    return {unit(p.x()), unit(p.y())};
}

} // namespace

TaskBarPlace::TaskBarPlace(QString path, QObject *parent) : QObject(parent), m_path(std::move(path)) {
    if (m_path.isEmpty())
        return;
    if (const Result<QJsonObject> read = readJsonObject(m_path)) {
        if (const QJsonObject place = read->value("place").toObject(); place.contains("x") && place.contains("y")) {
            m_has = true;
            m_place = held({place.value("x").toDouble(0.5), place.value("y").toDouble(0.5)});
            m_pinned = read->value("pinned").toBool(false);
        }
    }
}

void TaskBarPlace::setPlace(const QPointF &place) {
    const QPointF wanted = held(place);
    if (m_pinned || (m_has && wanted == m_place))
        return;
    m_has = true;
    m_place = wanted;
    save();
    emit changed();
}

void TaskBarPlace::setPinned(bool pinned) {
    if (pinned == m_pinned || (pinned && !m_has)) // a bar that was never moved has nothing to pin
        return;
    m_pinned = pinned;
    save();
    emit changed();
}

void TaskBarPlace::reset() {
    if (!m_has && !m_pinned)
        return;
    m_has = false;
    m_pinned = false;
    m_place = {0.5, 1};
    save();
    emit changed();
}

QPointF TaskBarPlace::positionIn(const QSizeF &area, const QSizeF &box, double margin) const {
    const place::Bounds bounds{area.width(), area.height(), box.width(), box.height(), margin};
    const place::Point p = place::fromFraction({m_place.x(), m_place.y()}, bounds);
    return {p.x, p.y};
}

void TaskBarPlace::save() const {
    if (m_path.isEmpty())
        return;
    if (!m_has) {
        QFile::remove(m_path);
        return;
    }
    writeJsonFile(m_path, QJsonObject{{"place", QJsonObject{{"x", m_place.x()}, {"y", m_place.y()}}}, {"pinned", m_pinned}});
}

} // namespace fugue
