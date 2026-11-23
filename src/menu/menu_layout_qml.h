#pragma once

// menus.h's bar layout rules for QML (#262): which menus an area's header holds for a view type, and the workflow tab
// strip's widths and scroll. No state, so any QML may call it, as AreaLayout.

#include <QObject>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class MenuLayout : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit MenuLayout(QObject *parent = nullptr) : QObject(parent) {}

    // {tree, prefix, needsView} of a view type's header menus; tree is "" for none.
    Q_INVOKABLE QVariantMap viewMenus(const QString &viewType) const;
    Q_INVOKABLE qreal tabWidth(int count, qreal room, qreal preferred, qreal minimum) const;
    Q_INVOKABLE qreal tabScroll(int current, int count, qreal width, qreal room, qreal offset) const;
};

} // namespace fugue
