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

QVariantMap ColorHistory::rgb(const QString &hex) const {
    const Rgb value = rgbFromHex(hex);
    return {{"r", value.r}, {"g", value.g}, {"b", value.b}};
}

QVariantMap ColorHistory::hsb(double h, double s, double v) const {
    const Hsb value = hsbRounded({h, s, v});
    return {{"h", value.h}, {"s", value.s}, {"b", value.b}};
}

} // namespace fugue
