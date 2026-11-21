#include "canvas/align.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace fugue::align {

namespace {

constexpr qreal kMinGap = 24; // world units, the room distribute always leaves between two nodes

// Positions are saved as whole numbers; a move smaller than that is no move.
QPointF whole(QPointF p) { return QPointF(std::round(p.x()), std::round(p.y())); }

void put(QHash<QString, QPointF> &moves, const Box &box, QPointF to) {
    to = whole(to);
    if (!box.pinned && to != whole(box.rect.topLeft()))
        moves.insert(box.id, to);
}

} // namespace

QHash<QString, QPointF> alignTo(const QList<Box> &boxes, Edge edge) {
    QHash<QString, QPointF> moves;
    if (boxes.size() < 2)
        return moves;
    qreal target = edge == Edge::Left || edge == Edge::Top ? std::numeric_limits<qreal>::max()
                                                           : std::numeric_limits<qreal>::lowest();
    for (const Box &box : boxes) {
        switch (edge) {
        case Edge::Left: target = std::min(target, box.rect.left()); break;
        case Edge::Right: target = std::max(target, box.rect.right()); break;
        case Edge::Top: target = std::min(target, box.rect.top()); break;
        case Edge::Bottom: target = std::max(target, box.rect.bottom()); break;
        }
    }
    for (const Box &box : boxes) {
        QPointF at = box.rect.topLeft();
        switch (edge) {
        case Edge::Left: at.setX(target); break;
        case Edge::Right: at.setX(target - box.rect.width()); break;
        case Edge::Top: at.setY(target); break;
        case Edge::Bottom: at.setY(target - box.rect.height()); break;
        }
        put(moves, box, at);
    }
    return moves;
}

QHash<QString, QPointF> distribute(const QList<Box> &boxes, Axis axis) {
    QHash<QString, QPointF> moves;
    if (boxes.size() < 3)
        return moves;
    const bool across = axis == Axis::Horizontal;
    const auto start = [across](const Box &box) { return across ? box.rect.left() : box.rect.top(); };
    const auto length = [across](const Box &box) { return across ? box.rect.width() : box.rect.height(); };

    std::vector<const Box *> order;
    for (const Box &box : boxes)
        order.push_back(&box);
    // The sort is stable, so nodes that start at the same place keep the order they were given in.
    std::stable_sort(order.begin(), order.end(), [&](const Box *a, const Box *b) { return start(*a) < start(*b); });

    qreal first = start(*order.front());
    qreal last = first;
    qreal total = 0;
    for (const Box *box : order) {
        last = std::max(last, start(*box) + length(*box));
        total += length(*box);
    }
    // Nodes that already overlap, or fill the span, would get a negative gap and stay on top of one
    // another: keep a minimum gap and let the run grow past the last node's end instead.
    const qreal gap = std::max(kMinGap, (last - first - total) / qreal(order.size() - 1));
    qreal at = first;
    for (const Box *box : order) {
        put(moves, *box, across ? QPointF(at, box->rect.top()) : QPointF(box->rect.left(), at));
        at += length(*box) + gap;
    }
    return moves;
}

QHash<QString, QPointF> arrange(const QList<Box> &boxes, const QString &mode) {
    if (mode == QLatin1String("left"))
        return alignTo(boxes, Edge::Left);
    if (mode == QLatin1String("right"))
        return alignTo(boxes, Edge::Right);
    if (mode == QLatin1String("top"))
        return alignTo(boxes, Edge::Top);
    if (mode == QLatin1String("bottom"))
        return alignTo(boxes, Edge::Bottom);
    if (mode == QLatin1String("horizontal"))
        return distribute(boxes, Axis::Horizontal);
    if (mode == QLatin1String("vertical"))
        return distribute(boxes, Axis::Vertical);
    return {};
}

} // namespace fugue::align
