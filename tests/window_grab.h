#pragma once

#include <QImage>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSignalSpy>

namespace test_support {

// What the window shows, popups included, drawn by the scene graph into an image of its
// own. QQuickWindow::grabWindow() reads the window's framebuffer back instead, and under
// QT_QUICK_BACKEND=rhi on the offscreen platform every window after the first in a
// process reads back garbage there (left-over pixels of earlier windows).
inline QImage grab(QQuickWindow *window) {
    const QSharedPointer<QQuickItemGrabResult> result = window->contentItem()->grabToImage();
    if (!result)
        return {};
    QSignalSpy ready(result.data(), &QQuickItemGrabResult::ready);
    return ready.wait(5000) ? result->image() : QImage();
}

} // namespace test_support
