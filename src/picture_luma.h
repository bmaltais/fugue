#pragma once

// How light the part of a picture under a control is, so a frosted control (src/kit/KitFrost.qml) picks dark or white ink and
// a tint that reads on it (docs/canvas-look.md "Drift kit"). A small downscaled copy of a picture is read once and cached (by
// path and modification time), and the mean is taken over a normalized rectangle of that copy: never per frame, never at full
// size. No window, no state beyond the cache, so any QML may ask.

#include <QHash>
#include <QImage>
#include <QObject>
#include <QRectF>
#include <QUrl>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue::luma {

// WCAG relative luminance (0 black, 1 white) of an sRGB color.
double relative(const QColor &color);

// The mean relative luminance (linear light) of `normalized` (x, y, width, height each 0 to 1, clamped to the image) of `image`;
// -1 for a null image or an empty rectangle. The image is read at most 64 px on its long side.
double mean(const QImage &image, const QRectF &normalized);

// Which ink reads on a patch of that mean luminance: pale patches (above `paleAbove`) take dark ink.
constexpr double paleAbove = 0.55;

} // namespace fugue::luma

namespace fugue {

class PictureLuma : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit PictureLuma(QObject *parent = nullptr) : QObject(parent) {}

    // The mean relative luminance under a rectangle (normalized) of a picture file or url; -1 when it cannot be read.
    Q_INVOKABLE double meanOfFile(const QUrl &source, double x, double y, double width, double height);
    // The same over an image QML holds (an `Item.grabToImage` result's `image`).
    Q_INVOKABLE double meanOfImage(const QVariant &image, double x, double y, double width, double height) const;
    Q_INVOKABLE double relativeLuminance(const QColor &color) const { return luma::relative(color); }
    Q_INVOKABLE double paleThreshold() const { return luma::paleAbove; }

private:
    QHash<QString, QImage> m_cache; // by path and modification time
};

} // namespace fugue
