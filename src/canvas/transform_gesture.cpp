#include "canvas/transform_gesture.h"

#include <cmath>

namespace fugue {

void TransformGesture::setGuides(GuidesModel *guides) {
    if (m_guides == guides)
        return;
    m_guides = guides;
    emit changed();
}

void TransformGesture::setSnapOptions(const QVariantMap &options) {
    if (m_options == options)
        return;
    m_options = options;
    emit changed();
}

canvas::LayerTransform TransformGesture::placementOf(const QVariantMap &map) {
    canvas::LayerTransform t;
    t.cx = map.value("cx").toDouble();
    t.cy = map.value("cy").toDouble();
    t.width = map.value("width").toDouble();
    t.height = map.value("height").toDouble();
    t.angle = map.value("angle").toDouble();
    t.flipX = map.value("flipX").toBool();
    t.flipY = map.value("flipY").toBool();
    canvas::filterFromName(map.value("filter").toString(), &t.filter); // an unknown name leaves Lanczos
    return t;
}

QVariantMap TransformGesture::mapOf(const canvas::LayerTransform &placement) {
    const canvas::LayerTransform t = placement.normalized();
    QVariantList corners;
    for (const crop::Point &p : xform::corners(t))
        corners.append(QVariantMap{{"x", p.x}, {"y", p.y}});
    const crop::RectF box = xform::bounds(t);
    return QVariantMap{{"cx", t.cx}, {"cy", t.cy}, {"width", t.width}, {"height", t.height}, {"angle", t.angle}, {"flipX", t.flipX}, {"flipY", t.flipY},
                       {"filter", canvas::filterName(t.filter)}, {"x", t.cx - std::round(t.width) / 2}, {"y", t.cy - std::round(t.height) / 2}, {"corners", corners},
                       {"bounds", QVariantMap{{"x", box.x}, {"y", box.y}, {"width", box.width}, {"height", box.height}}}};
}

QVariantMap TransformGesture::dragged(const QVariantMap &start, const QString &handleName, const QVariantMap &from, const QVariantMap &to, bool shift, bool alt, bool snap) const {
    const canvas::LayerTransform begin = placementOf(start);
    xform::Handle handle = xform::Handle::None;
    if (!xform::handleFromName(handleName, &handle))
        return mapOf(begin);
    const xform::Keys keys{shift, alt};
    canvas::LayerTransform t = xform::dragged(begin, handle, {from.value("x").toDouble(), from.value("y").toDouble()}, {to.value("x").toDouble(), to.value("y").toDouble()}, keys);
    if (snap && m_guides)
        t = xform::snapped(begin, t, handle, keys, m_guides->targets(m_options));
    return mapOf(t);
}

QVariantMap TransformGesture::nudged(const QVariantMap &start, double dx, double dy) const {
    canvas::LayerTransform t = placementOf(start);
    t.cx += std::round(dx);
    t.cy += std::round(dy);
    return mapOf(t);
}

QVariantMap TransformGesture::changed(const QVariantMap &start, const QVariantMap &changes, bool keepRatio) const {
    canvas::LayerTransform t = placementOf(start);
    const double ratio = t.height > 0 ? t.width / t.height : 1;
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const QString key = it.key();
        if (key == QLatin1String("cx"))
            t.cx = it.value().toDouble();
        else if (key == QLatin1String("cy"))
            t.cy = it.value().toDouble();
        else if (key == QLatin1String("width")) {
            t.width = it.value().toDouble();
            if (keepRatio)
                t.height = t.width / ratio;
        } else if (key == QLatin1String("height")) {
            t.height = it.value().toDouble();
            if (keepRatio)
                t.width = t.height * ratio;
        } else if (key == QLatin1String("angle"))
            t.angle = it.value().toDouble();
        else if (key == QLatin1String("flipX"))
            t.flipX = it.value().toBool();
        else if (key == QLatin1String("flipY"))
            t.flipY = it.value().toBool();
        else if (key == QLatin1String("filter"))
            canvas::filterFromName(it.value().toString(), &t.filter);
    }
    t.width = std::clamp(t.width, 1.0, double(canvas::maxTransformSide));
    t.height = std::clamp(t.height, 1.0, double(canvas::maxTransformSide));
    return mapOf(t);
}

QVariantMap TransformGesture::describe(const QVariantMap &placement) const { return mapOf(placementOf(placement)); }

} // namespace fugue
