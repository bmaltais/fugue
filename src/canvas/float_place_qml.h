#pragma once

// engine/float_place.h for QML (#120): where a floating bar sits in an area. No state, so any QML may
// call it (MoveGrip.qml, the run toolbar's panel). Sizes and places are plain numbers; a place is a
// point, a remembered place a point of fractions (0 to 1 of the room the box has).

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class FloatPlace : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit FloatPlace(QObject *parent = nullptr) : QObject(parent) {}

    // `area` and `box` are sizes, `margin` the gap kept from the area's edges.
    Q_INVOKABLE QPointF clamped(const QPointF &p, const QSizeF &area, const QSizeF &box, double margin) const;
    Q_INVOKABLE QPointF fromFraction(const QPointF &fraction, const QSizeF &area, const QSizeF &box, double margin) const;
    Q_INVOKABLE QPointF toFraction(const QPointF &p, const QSizeF &area, const QSizeF &box, double margin) const;
    Q_INVOKABLE QPointF snapped(const QPointF &p, const QSizeF &area, const QSizeF &box, double margin, double threshold) const;
    Q_INVOKABLE QPointF moved(const QPointF &p, double dx, double dy, const QSizeF &area, const QSizeF &box, double margin) const;
    // The panel: its top left, whether it hangs under the bar, and the height the side has for it.
    Q_INVOKABLE QPointF panelPlace(const QRectF &bar, const QSizeF &panel, const QSizeF &area, double gap, double sideHeight = -1) const;
    Q_INVOKABLE bool opensBelow(const QRectF &bar, double panelHeight, const QSizeF &area, double gap) const;
    Q_INVOKABLE double roomOnSide(const QRectF &bar, double panelHeight, const QSizeF &area, double gap) const;
};

} // namespace fugue
