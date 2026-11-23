#include "engine/canvas_layer_tree.h"

#include <algorithm>
#include <functional>

namespace fugue::canvas {

namespace {

constexpr QChar kSep = QChar(0x1f); // between the names of a key: not in a layer's name that a person types

QString pathText(const LayerPath &path) {
    QString out;
    for (int index : path)
        out += QString::number(index) + QLatin1Char('/');
    return out;
}

} // namespace

void LayerOutline::setTree(const Tree &tree) {
    m_all.clear();
    m_visible.clear();
    m_byId.clear();
    m_byPath.clear();
    m_keys.clear();
    m_top = int(tree.layers.size());
    // Ids: the top level first (id == position), then what is inside the groups, a group before its children.
    std::vector<std::pair<LayerPath, const LayerRecord *>> walk; // in id order
    for (int i = 0; i < m_top; ++i)
        walk.push_back({LayerPath{i}, &tree.layers[size_t(i)]});
    // The nested ones: top first, depth first.
    std::vector<std::pair<LayerPath, const LayerRecord *>> nested;
    const std::function<void(const LayerPath &, const LayerRecord &)> descend = [&](const LayerPath &path, const LayerRecord &group) {
        for (int c = int(group.children.size()) - 1; c >= 0; --c) {
            LayerPath child = path;
            child.push_back(c);
            nested.push_back({child, &group.children[size_t(c)]});
            descend(child, group.children[size_t(c)]);
        }
    };
    for (int i = m_top - 1; i >= 0; --i)
        descend(LayerPath{i}, tree.layers[size_t(i)]);
    walk.insert(walk.end(), nested.begin(), nested.end());

    // Rows, with the names above them for the open state's key.
    QHash<QString, int> ofPath;
    std::vector<const LayerRecord *> records;
    for (size_t k = 0; k < walk.size(); ++k) {
        const LayerPath &path = walk[k].first;
        const LayerRecord &layer = *walk[k].second;
        OutlineRow row;
        row.id = int(k);
        row.path = path;
        row.depth = int(path.size()) - 1;
        row.name = layer.name;
        row.group = layer.group;
        row.children = int(layer.children.size());
        QString key;
        bool shown = layer.visible, lockedAbove = false;
        if (path.size() > 1) {
            const LayerPath parent(path.begin(), path.end() - 1);
            const int parentId = ofPath.value(pathText(parent), -1);
            key = m_keys[size_t(parentId)] + kSep;
            shown = shown && m_all[size_t(parentId)].shown;
            lockedAbove = m_all[size_t(parentId)].lockedAbove || records[size_t(parentId)]->locked;
        }
        key += layer.name;
        row.shown = shown;
        row.lockedAbove = lockedAbove;
        ofPath.insert(pathText(path), row.id);
        m_byPath.insert(pathText(path), row.id);
        m_byId.insert(row.id, int(m_all.size()));
        m_keys.push_back(key);
        records.push_back(&layer);
        m_all.push_back(row);
    }
    listVisible();
}

void LayerOutline::listVisible() {
    for (OutlineRow &each : m_all)
        each.open = each.group && each.children > 0 && m_open.contains(m_keys[size_t(m_byId.value(each.id))]);
    m_visible.clear();
    // Top first, a group's children right under it.
    const std::function<void(const LayerPath &)> list = [&](const LayerPath &path) {
        const OutlineRow &r = m_all[size_t(m_byId.value(m_byPath.value(pathText(path))))];
        m_visible.push_back(r);
        if (r.open)
            for (int c = r.children - 1; c >= 0; --c) {
                LayerPath child = path;
                child.push_back(c);
                list(child);
            }
    };
    for (int i = m_top - 1; i >= 0; --i)
        list(LayerPath{i});
}

const OutlineRow *LayerOutline::row(int id) const {
    const auto it = m_byId.constFind(id);
    return it == m_byId.cend() ? nullptr : &m_all[size_t(it.value())];
}

LayerPath LayerOutline::pathOf(int id) const {
    const OutlineRow *found = row(id);
    return found ? found->path : LayerPath();
}

int LayerOutline::idOf(const LayerPath &path) const { return path.empty() ? -1 : m_byPath.value(pathText(path), -1); }

int LayerOutline::levelSize(const LayerPath &parent) const {
    if (parent.empty())
        return m_top;
    const OutlineRow *found = row(idOf(parent));
    return found && found->group ? found->children : 0;
}

bool LayerOutline::isOpen(const LayerPath &group) const {
    const OutlineRow *found = row(idOf(group));
    return found && found->open;
}

bool LayerOutline::setOpen(const LayerPath &group, bool open) {
    const OutlineRow *found = row(idOf(group));
    if (!found || !found->group || found->children == 0)
        return false;
    const QString &key = m_keys[size_t(m_byId.value(found->id))];
    if (open == m_open.contains(key))
        return true;
    if (open)
        m_open.insert(key);
    else
        m_open.remove(key);
    listVisible();
    return true;
}

bool LayerOutline::toggle(const LayerPath &group) {
    const OutlineRow *found = row(idOf(group));
    return found && setOpen(group, !found->open);
}

void LayerOutline::setAllOpen(bool open) {
    std::vector<LayerPath> groups;
    for (const OutlineRow &each : m_all)
        if (each.group && each.children > 0)
            groups.push_back(each.path);
    for (const LayerPath &path : groups)
        setOpen(path, open);
}

void LayerOutline::renameOpen(const LayerPath &group, const QString &oldName) {
    const OutlineRow *found = row(idOf(group));
    if (!found)
        return;
    const QString newKey = m_keys[size_t(m_byId.value(found->id))];
    const QString oldKey = newKey.left(newKey.size() - found->name.size()) + oldName;
    if (oldKey == newKey)
        return;
    QSet<QString> moved;
    for (const QString &key : std::as_const(m_open)) {
        if (key == oldKey)
            moved.insert(newKey);
        else if (key.startsWith(oldKey + kSep))
            moved.insert(newKey + key.mid(oldKey.size()));
        else
            moved.insert(key);
    }
    m_open = moved;
    listVisible();
}

std::vector<int> LayerOutline::fitSelection(const std::vector<int> &ids) const {
    std::vector<int> out;
    for (int id : ids) {
        const OutlineRow *found = row(id);
        if (!found)
            continue;
        LayerPath path = found->path;
        // The highest group above that is closed hides it.
        for (size_t depth = 1; depth < path.size(); ++depth) {
            const LayerPath above(path.begin(), path.begin() + long(depth));
            if (!isOpen(above)) {
                path = above;
                break;
            }
        }
        const int fitted = idOf(path);
        if (std::find(out.begin(), out.end(), fitted) == out.end())
            out.push_back(fitted);
    }
    return out;
}

bool LayerOutline::levelOf(const std::vector<int> &ids, LayerPath *parent, std::vector<int> *indices) const {
    if (ids.empty())
        return false;
    LayerPath level;
    std::vector<int> places;
    for (size_t i = 0; i < ids.size(); ++i) {
        const LayerPath path = pathOf(ids[i]);
        if (path.empty())
            return false;
        const LayerPath above(path.begin(), path.end() - 1);
        if (i == 0)
            level = above;
        else if (above != level)
            return false;
        places.push_back(path.back());
    }
    if (parent)
        *parent = level;
    if (indices)
        *indices = places;
    return true;
}

std::vector<int> LayerOutline::rangeBetween(int a, int b) const {
    int from = -1, to = -1;
    for (int i = 0; i < int(m_visible.size()); ++i) {
        if (m_visible[size_t(i)].id == a)
            from = i;
        if (m_visible[size_t(i)].id == b)
            to = i;
    }
    std::vector<int> out;
    if (from < 0 || to < 0)
        return out;
    for (int i = std::min(from, to); i <= std::max(from, to); ++i)
        out.push_back(m_visible[size_t(i)].id);
    return out;
}

DropPlace LayerOutline::dropAt(const std::vector<int> &ids, int gap) const {
    DropPlace place;
    std::vector<int> dragged;
    if (!levelOf(ids, &place.parent, &dragged)) {
        place.why = QStringLiteral("Drag layers of one group at a time.");
        return place;
    }
    const int count = int(m_visible.size());
    gap = std::clamp(gap, 0, count);
    // The listed rows of the dragged layers' own level, with where each one's rows (its own and the open group's children) start.
    const size_t level = place.parent.size() + 1;
    std::vector<int> starts, indexOf; // top first
    int end = -1;                     // where the level's last row ends
    for (int i = 0; i < count; ++i) {
        const OutlineRow &r = m_visible[size_t(i)];
        if (r.path.size() == level && std::equal(place.parent.begin(), place.parent.end(), r.path.begin())) {
            starts.push_back(i);
            indexOf.push_back(r.path.back());
        }
        if (r.path.size() >= level && std::equal(place.parent.begin(), place.parent.end(), r.path.begin()))
            end = i + 1;
    }
    int rank = -1; // among the level's rows, top first: the line sits above that one (the count: below the last)
    for (size_t k = 0; k < starts.size(); ++k)
        if (starts[k] == gap)
            rank = int(k);
    if (rank < 0 && !starts.empty() && gap == end)
        rank = int(starts.size());
    if (rank < 0) {
        place.why = QStringLiteral("Drop between the layers of the same group: moving a layer into or out of a group is not offered yet.");
        return place;
    }
    int below = 0; // the layers that are not dragged and lie under the line
    for (size_t k = size_t(rank); k < indexOf.size(); ++k)
        if (std::find(dragged.begin(), dragged.end(), indexOf[k]) == dragged.end())
            ++below;
    place.ok = true;
    place.to = below;
    return place;
}

} // namespace fugue::canvas
