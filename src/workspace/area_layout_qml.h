#pragma once

// workspace/area_tree.h for QML (#217): the default tree and the layout of one branch, for AreaNode.qml. No
// state, so any QML may call it, as FloatPlace. A tree and a node are their saved form (AreaTree::toJson) as a
// JS object; a rectangle is in the host item's coordinates.

#include <QObject>
#include <QRectF>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class ViewRegistry;

class AreaLayout : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit AreaLayout(QObject *parent = nullptr) : QObject(parent) {}

    // The built-in tree, {"format": 1, "root": {...}}: one area, id 1, showing the Graph.
    Q_INVOKABLE QVariantMap defaultTree() const;
    // The rectangles of a branch's children in `rect`, in child order, each child kept at its views' minimum
    // sizes (`registry`'s; none: no minimum) while they fit, mirrored along a row for `rightToLeft`. Empty for a
    // leaf, or for a node that does not validate.
    Q_INVOKABLE QVariantList childRects(const QVariantMap &node, const QRectF &rect, ViewRegistry *registry,
                                        bool rightToLeft) const;
    // What makes two nodes the same shape: leaf ids, views and axes, not sizes ("" for a node that does not
    // validate). A change of sizes alone moves the views; a change of shape makes them again.
    Q_INVOKABLE QString shapeOf(const QVariantMap &node) const;
};

} // namespace fugue
