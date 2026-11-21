#include "menu/color_history.h"

namespace fugue {

bool ColorHistory::use(const QString &color) {
    if (normalizeHex(color).isEmpty())
        return false;
    m_recent.use(color);
    emit recentChanged();
    return true;
}

QVariantMap ColorHistory::hsv(const QString &hex) const {
    const Hsv value = hsvFromHex(hex);
    return {{"h", value.h}, {"s", value.s}, {"v", value.v}};
}

} // namespace fugue
