#include "canvas/crop_qml.h"

#include "engine/crop_geometry.h"

namespace fugue {

namespace {

crop::Rect rectOf(const QVariantMap &map) {
    return {map.value("x").toInt(), map.value("y").toInt(), map.value("width").toInt(), map.value("height").toInt()};
}

QVariantMap mapOf(const crop::Rect &r) {
    return {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}};
}

crop::Size sizeOf(const QSize &s) { return {s.width(), s.height()}; }

const char *const kHandles[] = {"none", "inside", "top-left", "top", "top-right", "right", "bottom-right", "bottom", "bottom-left", "left"};

crop::Handle handleOf(const QString &name) {
    for (int i = 0; i < 10; ++i)
        if (name == QLatin1String(kHandles[i]))
            return crop::Handle(i);
    return crop::Handle::None;
}

} // namespace

QVariantMap CropGeometry::applied(const QVariantMap &rect, const QSize &image) const {
    return mapOf(crop::applied(rectOf(rect), sizeOf(image)));
}

QVariantMap CropGeometry::clamped(const QVariantMap &rect, const QSize &image) const {
    return mapOf(crop::clamped(rectOf(rect), sizeOf(image)));
}

QVariantMap CropGeometry::moved(const QVariantMap &rect, int dx, int dy, const QSize &image) const {
    return mapOf(crop::moved(rectOf(rect), dx, dy, sizeOf(image)));
}

QVariantMap CropGeometry::resized(const QVariantMap &rect, const QString &handle, int dx, int dy, const QSize &image,
                                  double ratio, bool fromCenter) const {
    return mapOf(crop::resized(rectOf(rect), handleOf(handle), dx, dy, sizeOf(image), ratio, fromCenter));
}

QVariantMap CropGeometry::drawn(int ax, int ay, int bx, int by, const QSize &image, double ratio, bool fromCenter) const {
    return mapOf(crop::drawn(ax, ay, bx, by, sizeOf(image), ratio, fromCenter));
}

QVariantMap CropGeometry::fitRatio(const QVariantMap &rect, double ratio, const QSize &image) const {
    return mapOf(crop::fitRatio(rectOf(rect), ratio, sizeOf(image)));
}

double CropGeometry::ratioOf(const QString &name, const QSize &image) const {
    return crop::ratioOf(name.toStdString(), sizeOf(image));
}

QString CropGeometry::handleAt(const QVariantMap &rect, double px, double py, double grab) const {
    return QLatin1String(kHandles[int(crop::handleAt(rectOf(rect), px, py, grab))]);
}

} // namespace fugue
