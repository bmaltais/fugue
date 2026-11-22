#pragma once

// What a scrolling surface owes its user, in the real window (offscreen): the wheel scrolls it, the
// scroll bar's thumb drags it both ways, and no content lies under the bar while the bar shows.
// FugueScrollBar overlays the view's edge, so each host pads its content by the bar's `inset`.

#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

namespace scroll_checks {

inline QPoint centerOf(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

// A real wheel event through the window at a scene point (a notch is 120; negative scrolls down).
// `pixelY` is a touchpad's own delta (a mouse has none).
inline void wheelAt(QWindow *window, QPoint at, int angleY, int pixelY = 0) {
    QWheelEvent event(QPointF(at), QPointF(window->mapToGlobal(at)), QPoint(0, pixelY), QPoint(0, angleY), Qt::NoButton,
                      Qt::NoModifier, pixelY != 0 ? Qt::ScrollUpdate : Qt::NoScrollPhase, false);
    QGuiApplication::sendEvent(window, &event);
}

// What an ordinary mouse wheel sends on the owner's Wayland session (seen there): a device of type
// TouchPad, no scroll phase, an angle of 120 per notch and a pixel delta of 15 (the same sign).
inline void waylandWheelAt(QWindow *window, QPoint at, int notches) {
    static QPointingDevice pad(QStringLiteral("wayland pointer"), 1, QInputDevice::DeviceType::TouchPad,
                               QPointingDevice::PointerType::Generic, QInputDevice::Capability::Position
                                   | QInputDevice::Capability::Scroll | QInputDevice::Capability::PixelScroll,
                               1, 0);
    const int sign = notches < 0 ? -1 : 1;
    QWheelEvent event(QPointF(at), QPointF(window->mapToGlobal(at)), QPoint(0, 15 * sign * qAbs(notches)),
                      QPoint(0, 120 * notches), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false,
                      Qt::MouseEventNotSynthesized, &pad);
    QGuiApplication::sendEvent(window, &event);
}

// The scroll bar of a view, or null.
inline QQuickItem *barOf(QQuickItem *view) {
    for (QQuickItem *child : view->childItems())
        if (child->objectName() == "fugueScrollBar")
            return child;
    return view->findChild<QQuickItem *>("fugueScrollBar");
}

// Presses the thumb, drags it `pixels` along the bar and lets go, with real mouse events. The
// view's `contentY` (or `contentX` for a horizontal bar) is read before and after by the caller.
inline void dragThumb(QWindow *window, QQuickItem *bar, int pixels) {
    QQuickItem *thumb = bar->property("contentItem").value<QQuickItem *>();
    const bool vertical = bar->property("vertical").toBool();
    const QPoint grab = centerOf(thumb);
    const QPoint step = vertical ? QPoint(0, pixels > 0 ? 4 : -4) : QPoint(pixels > 0 ? 4 : -4, 0);
    QTest::mousePress(window, Qt::LeftButton, {}, grab);
    QPoint at = grab;
    for (int moved = 0; moved < qAbs(pixels); moved += 4) {
        at += step;
        QTest::mouseMove(window, at);
    }
    QTest::mouseRelease(window, Qt::LeftButton, {}, at);
}

// Whether the wheel and a thumb drag both move `view`: scrolls it to the top, wheels down, drags
// the thumb down and up. Returns what failed, empty when all is well.
inline QString scrollsByWheelAndThumb(QWindow *window, QQuickItem *view) {
    QQuickItem *bar = barOf(view);
    if (!bar || !bar->isVisible())
        return QStringLiteral("no scroll bar showing");
    const bool vertical = bar->property("vertical").toBool();
    const char *axis = vertical ? "contentY" : "contentX";
    const auto at = [&] { return view->property(axis).toDouble(); };
    view->setProperty(axis, 0.0);
    QTest::qWait(20);
    wheelAt(window, centerOf(view), -120);
    QTest::qWait(30);
    if (!(at() > 0))
        return QStringLiteral("the wheel did not scroll");
    view->setProperty(axis, 0.0);
    QTest::qWait(20);
    waylandWheelAt(window, centerOf(view), -1);
    for (int waited = 0; waited < 600 && !(at() > 0); waited += 20)
        QTest::qWait(20);
    if (!(at() > 0))
        return QStringLiteral("the wheel as the Wayland session sends it (a TouchPad, no phase) did not scroll");
    view->setProperty(axis, 0.0);
    QTest::qWait(20);
    dragThumb(window, bar, 40);
    QTest::qWait(30);
    const qreal down = at();
    if (!(down > 0))
        return QStringLiteral("dragging the thumb did not scroll");
    dragThumb(window, bar, -40);
    QTest::qWait(30);
    if (!(at() < down))
        return QStringLiteral("dragging the thumb back did not scroll back");
    return {};
}

// Every item below `item` in the visual tree (delegates are not QObject children of their view).
inline void collect(QQuickItem *item, QList<QQuickItem *> &out) {
    for (QQuickItem *child : item->childItems()) {
        out << child;
        collect(child, out);
    }
}

// The text, image and button-like leaves of `host` that lie under the bar's rectangle: with the bar
// showing, there must be none. Returns the first one's description, empty when none.
inline QString contentUnderBar(QQuickItem *host, QQuickItem *bar) {
    if (!bar || !bar->isVisible())
        return QStringLiteral("no scroll bar showing");
    const QRectF barRect(bar->mapToScene(QPointF(0, 0)), bar->size());
    QList<QQuickItem *> all;
    collect(host, all);
    for (QQuickItem *item : all) {
        if (item == bar || bar->isAncestorOf(item) || !item->isVisible() || item->width() <= 0 || item->height() <= 0)
            continue;
        const QByteArray type = item->metaObject()->className();
        if (!type.startsWith("QQuickText") && !type.startsWith("QQuickImage"))
            continue;
        if (item->property("text").isValid() && item->property("text").toString().isEmpty())
            continue;
        // A clipped-away item (scrolled out of the view) is not under the bar.
        const QRectF rect(item->mapToScene(QPointF(0, 0)), item->size());
        const QRectF viewRect(host->mapToScene(QPointF(0, 0)), host->size());
        if (!viewRect.intersects(rect))
            continue;
        const QRectF overlap = rect.intersected(barRect);
        if (overlap.width() > 0.5 && overlap.height() > 0.5)
            return QStringLiteral("%1 \"%2\" at %3,%4 %5x%6 under the bar").arg(QString::fromLatin1(type), item->property("text").toString())
                .arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());
    }
    return {};
}

} // namespace scroll_checks
