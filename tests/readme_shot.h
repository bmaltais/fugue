#pragma once

// Helpers for the tests that save the README's pictures (#94, docs/recording-screenshots.md).

#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>

#include "window_grab.h"

namespace readme_shot {

// The part of the window the canvas shows its nodes in, trimmed to what is drawn on it plus
// `margin` pixels: no menu bar, no zoom badge, no empty canvas around the graph. `skipTop` leaves out
// that many pixels at the top of the canvas (the breadcrumb of an entered group).
inline QImage graphOnly(QQuickWindow *window, QQuickItem *canvas, int margin = 28, int skipTop = 0) {
    const QImage shot = test_support::grab(window).convertToFormat(QImage::Format_ARGB32);
    const QRect area = QRectF(canvas->mapToScene(QPointF(0, 0)), QSizeF(canvas->width(), canvas->height())).toRect()
                           .intersected(shot.rect());
    const QRgb background = shot.pixel(area.topLeft() + QPoint(4, 4));
    const auto differs = [&](QRgb pixel) {
        return qAbs(qRed(pixel) - qRed(background)) > 6 || qAbs(qGreen(pixel) - qGreen(background)) > 6
               || qAbs(qBlue(pixel) - qBlue(background)) > 6;
    };
    QRect box;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x) {
            if (x < area.left() + 110 && y > area.bottom() - 90) // the zoom badge
                continue;
            if (y < area.top() + skipTop) // the breadcrumb inside a group
                continue;
            if (differs(shot.pixel(x, y)))
                box = box.isNull() ? QRect(x, y, 1, 1) : box.united(QRect(x, y, 1, 1));
        }
    return shot.copy(box.adjusted(-margin, -margin, margin, margin).intersected(area));
}

} // namespace readme_shot
