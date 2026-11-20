#pragma once

// The links in an input value (4.2), for code that rewires a graph: the canvas's edits and
// the group edits (engine/groups.h).

#include "engine/document.h"

#include <QList>

#include <utility>

namespace fugue {

// Every link in an input value, with the list item it fills (-1: the whole value).
inline QList<std::pair<PortRef, int>> linksIn(const InputValue &value) {
    QList<std::pair<PortRef, int>> found;
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
InputValue withoutLinks(const InputValue &value, Drop drop) {
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

// The value with every link's source passed through `map` (which returns it unchanged to
// keep it).
template <typename Map>
InputValue withLinksMapped(const InputValue &value, Map map) {
    if (value.kind() == InputValue::Kind::Link)
        return InputValue::link(map(value.from()));
    if (value.kind() != InputValue::Kind::List)
        return value;
    QList<InputValue> items = value.items();
    for (InputValue &item : items)
        if (item.kind() == InputValue::Kind::Link)
            item = InputValue::link(map(item.from()));
    return InputValue::list(items);
}

// Takes the links `drop` picks out of every input of the node. True when any went.
template <typename Drop>
bool stripLinks(Node &node, Drop drop) {
    bool changed = false;
    for (auto in = node.inputs.begin(); in != node.inputs.end();) {
        const InputValue kept = withoutLinks(in.value(), drop);
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

} // namespace fugue
