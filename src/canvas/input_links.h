#pragma once

// Small helpers the canvas model's files share: the links in an input value, and a layout
// point.

#include "engine/document.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QPointF>

#include <optional>
#include <utility>

namespace canvas_detail {

// Every link in an input value, with the list item it fills (-1: the whole value).
inline QList<std::pair<fugue::PortRef, int>> linksIn(const fugue::InputValue &value) {
    using fugue::InputValue;
    QList<std::pair<fugue::PortRef, int>> found;
    if (value.kind() == InputValue::Kind::Link) {
        found.append({value.from(), -1});
    } else if (value.kind() == InputValue::Kind::List) {
        const QList<InputValue> items = value.items();
        for (int i = 0; i < items.size(); ++i)
            if (items[i].kind() == InputValue::Kind::Link)
                found.append({items[i].from(), i});
    }
    return found;
}

// The value with the links `drop` picks taken out: a single link goes, a list loses those
// items. Empty when nothing is left (the input then uses its default).
template <typename Drop>
fugue::InputValue withoutLinks(const fugue::InputValue &value, Drop drop) {
    using fugue::InputValue;
    if (value.kind() == InputValue::Kind::Link)
        return drop(value.from()) ? InputValue() : value;
    if (value.kind() != InputValue::Kind::List)
        return value;
    QList<InputValue> items;
    for (const InputValue &item : value.items())
        if (item.kind() != InputValue::Kind::Link || !drop(item.from()))
            items.append(item);
    if (items.size() == value.items().size())
        return value;
    return items.isEmpty() ? InputValue() : InputValue::list(items);
}

// Takes the links `drop` picks out of every input of the node. True when any went.
template <typename Drop>
bool stripLinks(fugue::Node &node, Drop drop) {
    bool changed = false;
    for (auto in = node.inputs.begin(); in != node.inputs.end();) {
        const fugue::InputValue kept = withoutLinks(in.value(), drop);
        if (kept == in.value()) {
            ++in;
            continue;
        }
        changed = true;
        if (kept.isEmpty()) {
            in = node.inputs.erase(in);
        } else {
            in.value() = kept;
            ++in;
        }
    }
    return changed;
}

inline std::optional<QPointF> point(const QJsonValue &value) {
    const QJsonArray pair = value.toArray();
    if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble())
        return std::nullopt;
    return QPointF(pair[0].toDouble(), pair[1].toDouble());
}

} // namespace canvas_detail
