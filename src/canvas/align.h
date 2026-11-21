#pragma once

// The math of Align and Distribute (#58, ComfyUI's Align Selected To and Distribute Nodes):
// where each selected node goes, from the boxes it has now. No model, no undo; the canvas
// model applies the result as one edit (CanvasModel::alignSelection).

#include <QHash>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace fugue::align {

struct Box {
    QString id;
    QRectF rect;         // as drawn, in world units
    bool pinned = false; // pinned in place (#55): never moved, but still counts for the edge
};

enum class Edge { Left, Right, Top, Bottom };
enum class Axis { Horizontal, Vertical };

// The new top left of each node that has to move, to share the selection's outermost `edge`.
QHash<QString, QPointF> alignTo(const QList<Box> &boxes, Edge edge);

// The new top left of each node that has to move, so the gaps between neighbours along `axis`
// are equal. The span from the first start to the furthest end stays; the nodes keep their order. A pinned node stays
// where it is, so the gaps around it are not equal. Fewer than three boxes: nothing moves.
QHash<QString, QPointF> distribute(const QList<Box> &boxes, Axis axis);

// The editor's mode names: "left", "right", "top", "bottom", "horizontal", "vertical".
// Moves for the selection, empty for a name it does not know.
QHash<QString, QPointF> arrange(const QList<Box> &boxes, const QString &mode);

} // namespace fugue::align
