#include "canvas/float_place_qml.h"

#include "engine/float_place.h"

namespace fugue {

namespace {

place::Bounds boundsOf(const QSizeF &area, const QSizeF &box, double margin) {
    return {area.width(), area.height(), box.width(), box.height(), margin};
}

QPointF pointOf(const place::Point &p) { return {p.x, p.y}; }

place::Rect rectOf(const QRectF &r) { return {r.x(), r.y(), r.width(), r.height()}; }

place::Size sizeOf(const QSizeF &s) { return {s.width(), s.height()}; }

} // namespace

QPointF FloatPlace::clamped(const QPointF &p, const QSizeF &area, const QSizeF &box, double margin) const {
    return pointOf(place::clamped({p.x(), p.y()}, boundsOf(area, box, margin)));
}

QPointF FloatPlace::fromFraction(const QPointF &fraction, const QSizeF &area, const QSizeF &box, double margin) const {
    return pointOf(place::fromFraction({fraction.x(), fraction.y()}, boundsOf(area, box, margin)));
}

QPointF FloatPlace::toFraction(const QPointF &p, const QSizeF &area, const QSizeF &box, double margin) const {
    const place::Frac f = place::toFraction({p.x(), p.y()}, boundsOf(area, box, margin));
    return {f.x, f.y};
}

QPointF FloatPlace::snapped(const QPointF &p, const QSizeF &area, const QSizeF &box, double margin, double threshold) const {
    return pointOf(place::snapped({p.x(), p.y()}, boundsOf(area, box, margin), threshold));
}

QPointF FloatPlace::moved(const QPointF &p, double dx, double dy, const QSizeF &area, const QSizeF &box, double margin) const {
    return pointOf(place::moved({p.x(), p.y()}, dx, dy, boundsOf(area, box, margin)));
}

QPointF FloatPlace::panelPlace(const QRectF &bar, const QSizeF &panel, const QSizeF &area, double gap, double sideHeight) const {
    return pointOf(place::panelPlace(rectOf(bar), sizeOf(panel), sizeOf(area), gap, sideHeight));
}

bool FloatPlace::opensBelow(const QRectF &bar, double panelHeight, const QSizeF &area, double gap) const {
    return place::opensBelow(rectOf(bar), panelHeight, sizeOf(area), gap);
}

double FloatPlace::roomOnSide(const QRectF &bar, double panelHeight, const QSizeF &area, double gap) const {
    const place::Rect r = rectOf(bar);
    return place::opensBelow(r, panelHeight, sizeOf(area), gap) ? place::roomBelow(r, sizeOf(area), gap) : place::roomAbove(r, gap);
}

} // namespace fugue
