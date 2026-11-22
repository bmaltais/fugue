#include "workspace/area_layout_qml.h"

#include <QJsonObject>

#include "workspace/area_tree.h"
#include "workspace/view_registry.h"

namespace fugue {

namespace {

QString shape(const AreaTree::Node &node) {
    if (node.leaf)
        return QStringLiteral("%1:%2").arg(node.area.id).arg(node.area.view);
    QString text = node.axis == AreaAxis::Row ? QStringLiteral("row(") : QStringLiteral("column(");
    for (size_t i = 0; i < node.children.size(); ++i)
        text += (i ? QStringLiteral(",") : QString()) + shape(node.children[i]);
    return text + QLatin1Char(')');
}

} // namespace

QVariantMap AreaLayout::defaultTree() const { return AreaTree().toJson().toVariantMap(); }

QVariantList AreaLayout::childRects(const QVariantMap &node, const QRectF &rect, ViewRegistry *registry,
                                    bool rightToLeft) const {
    AreaTree::Node read;
    QVariantList rects;
    if (!AreaTree::nodeFromJson(QJsonObject::fromVariantMap(node), &read, nullptr, false))
        return rects;
    const AreaTree::MinimumSize minimum = [registry](const QString &view) {
        return registry ? registry->minimumSize(view) : QSizeF();
    };
    for (const QRectF &r : AreaTree::childRects(read, rect, minimum, rightToLeft))
        rects << r;
    return rects;
}

QString AreaLayout::shapeOf(const QVariantMap &node) const {
    AreaTree::Node read;
    return AreaTree::nodeFromJson(QJsonObject::fromVariantMap(node), &read, nullptr, false) ? shape(read) : QString();
}

} // namespace fugue
