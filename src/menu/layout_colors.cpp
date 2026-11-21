#include "menu/layout_colors.h"

#include <QJsonArray>
#include <QJsonObject>

#include "canvas/canvas_model.h"
#include "engine/layout_color.h"
#include "menu/menu_commands.h"

namespace fugue {

bool LayoutColors::apply(const QString &hex, const QString &frame) {
    const QString color = normalizeHex(hex);
    if (color.isEmpty())
        return false;
    applyLayoutColor(m_canvas, color, frame);
    return true;
}

QString LayoutColors::current(const QString &frame) const {
    const Document document = m_canvas.scopeDocument();
    QString color;
    if (!frame.isEmpty()) {
        for (const QJsonValue &entry : document.layout.value("frames").toArray())
            if (entry.toObject().value("id").toString() == frame)
                color = entry.toObject().value("color").toString();
    } else if (const QStringList selected = m_canvas.selectedIds(); !selected.isEmpty()) {
        color = document.layout.value("nodes").toObject().value(selected.first()).toObject().value("color").toString();
    }
    return isHexColor(color) ? color.toLower() : QString();
}

} // namespace fugue
