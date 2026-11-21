#pragma once

// engine/crop_geometry.h for QML (#73): the frame's moves and resizes, the ratios and the hit test, on
// plain objects ({x, y, width, height} for a rectangle, a `size` for the image, a handle as its
// name: "top-left", "top", "top-right", "right", "bottom-right", "bottom", "bottom-left", "left",
// "inside" or "none"). No state, so any QML may call it (CropFrame.qml, the crop window, a later
// mask or region picker).

#include <QObject>
#include <QSize>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class CropGeometry : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit CropGeometry(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE QVariantMap applied(const QVariantMap &rect, const QSize &image) const;
    Q_INVOKABLE QVariantMap clamped(const QVariantMap &rect, const QSize &image) const;
    Q_INVOKABLE QVariantMap moved(const QVariantMap &rect, int dx, int dy, const QSize &image) const;
    Q_INVOKABLE QVariantMap resized(const QVariantMap &rect, const QString &handle, int dx, int dy, const QSize &image,
                                    double ratio = 0, bool fromCenter = false) const;
    Q_INVOKABLE QVariantMap fitRatio(const QVariantMap &rect, double ratio, const QSize &image) const;
    // The ratio named "free", "source" or "w:h" as width / height (0 for free or a bad name).
    Q_INVOKABLE double ratioOf(const QString &name, const QSize &image) const;
    Q_INVOKABLE QString handleAt(const QVariantMap &rect, double px, double py, double grab) const;
};

} // namespace fugue
