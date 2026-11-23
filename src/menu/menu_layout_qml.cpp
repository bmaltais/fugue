#include "menu/menu_layout_qml.h"

#include "menu/menus.h"

namespace fugue {

QVariantMap MenuLayout::viewMenus(const QString &viewType) const {
    const menus::ViewMenus found = menus::viewMenus(viewType);
    return {{"tree", found.tree}, {"prefix", found.prefix}, {"needsView", found.needsView}};
}

qreal MenuLayout::tabWidth(int count, qreal room, qreal preferred, qreal minimum) const {
    return menus::tabWidth(count, room, preferred, minimum);
}

qreal MenuLayout::tabScroll(int current, int count, qreal width, qreal room, qreal offset) const {
    return menus::tabScroll(current, count, width, room, offset);
}

} // namespace fugue
