#include "workspace/area_host.h"

#include <QPointerEvent>

namespace fugue {

AreaHost::AreaHost(QQuickItem *parent) : QQuickItem(parent) { setFiltersChildMouseEvents(true); }

bool AreaHost::childMouseEventFilter(QQuickItem *item, QEvent *event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::TouchBegin:
    case QEvent::TabletPress: {
        auto *pointer = static_cast<QPointerEvent *>(event);
        if (pointer->pointCount() > 0)
            emit pressed(mapFromScene(pointer->point(0).scenePosition()));
        break;
    }
    default:
        break;
    }
    return QQuickItem::childMouseEventFilter(item, event);
}

} // namespace fugue
