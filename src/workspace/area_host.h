#pragma once

// The item the window's areas are drawn in while the window has more than one (#217 slice 1, Main.qml): views,
// headers and border handles are its children (AreaNode.qml). It tells which point of it was pressed, before the
// press reaches the item under it and without taking it, so a press anywhere in an area (the Graph, a Canvas,
// a header) makes that area the active one while the press still does what it does there.

#include <QPointF>
#include <QQuickItem>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class AreaHost : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit AreaHost(QQuickItem *parent = nullptr);

signals:
    // A mouse button, a touch or a pen went down at `point` (this item's coordinates).
    void pressed(QPointF point);

protected:
    bool childMouseEventFilter(QQuickItem *item, QEvent *event) override;
};

} // namespace fugue
