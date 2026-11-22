#include "workspace/area_tree.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>

#include "engine/json.h"

namespace fugue {

namespace {

const QString graphView = QStringLiteral("graph");

QString axisName(AreaAxis axis) { return axis == AreaAxis::Row ? QStringLiteral("row") : QStringLiteral("column"); }

AreaTree::Node leafNode(int id, const QString &view) {
    AreaTree::Node node;
    node.leaf = true;
    node.area.id = id;
    node.area.view = view;
    return node;
}

// Along a branch's axis: a size's length, and across it.
double along(AreaAxis axis, const QSizeF &size) { return axis == AreaAxis::Row ? size.width() : size.height(); }
double across(AreaAxis axis, const QSizeF &size) { return axis == AreaAxis::Row ? size.height() : size.width(); }

void collectIds(const AreaTree::Node &node, QList<int> &into) {
    if (node.leaf) {
        into << node.area.id;
        return;
    }
    for (const auto &child : node.children)
        collectIds(child, into);
}

bool findPath(const AreaTree::Node &node, int id, AreaTree::Path &path) {
    if (node.leaf)
        return node.area.id == id;
    for (int i = 0; i < int(node.children.size()); ++i) {
        path << i;
        if (findPath(node.children[size_t(i)], id, path))
            return true;
        path.removeLast();
    }
    return false;
}

AreaTree::Node *nodeAt(AreaTree::Node &root, const AreaTree::Path &path) {
    AreaTree::Node *at = &root;
    for (const int index : path) {
        if (at->leaf || index < 0 || index >= int(at->children.size()))
            return nullptr;
        at = &at->children[size_t(index)];
    }
    return at;
}

// Shares scaled to sum to 1, a branch of one child replaced by it, and a branch inside a branch of the same
// axis merged into it: what VS Code's grid keeps too (levels alternate their axis).
void normalize(AreaTree::Node &node) {
    if (node.leaf)
        return;
    for (auto &child : node.children)
        normalize(child);
    std::vector<AreaTree::Node> flat;
    for (auto &child : node.children) {
        if (!child.leaf && child.axis == node.axis) {
            for (auto &grandchild : child.children) {
                grandchild.size *= child.size;
                flat.push_back(std::move(grandchild));
            }
        } else {
            flat.push_back(std::move(child));
        }
    }
    node.children = std::move(flat);
    double total = 0;
    for (const auto &child : node.children)
        total += child.size;
    for (auto &child : node.children)
        child.size = total > 0 ? child.size / total : 1.0 / double(node.children.size());
    if (node.children.size() == 1) {
        const double size = node.size;
        AreaTree::Node only = std::move(node.children.front());
        node = std::move(only);
        node.size = size;
    }
}

bool isInteger(const QJsonValue &value) {
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number == std::floor(number);
}

struct Reader {
    QSet<int> ids;
    QString problem;

    bool fail(const QString &why) {
        problem = why;
        return false;
    }

    bool read(const QJsonObject &json, AreaTree::Node &node, int depth) {
        if (depth > AreaTree::kMaxDepth)
            return fail(QStringLiteral("the tree is deeper than %1").arg(AreaTree::kMaxDepth));
        const QString type = json.value("type").toString();
        node.extra = json;
        node.extra.remove("type");
        if (type == QLatin1String("leaf")) {
            const QJsonValue id = json.value("area");
            if (!isInteger(id) || id.toDouble() < 1 || id.toDouble() > 1e9)
                return fail(QStringLiteral("an area id is not a positive whole number"));
            node.leaf = true;
            node.area.id = id.toInt();
            if (ids.contains(node.area.id))
                return fail(QStringLiteral("area %1 is there twice").arg(node.area.id));
            ids.insert(node.area.id);
            if (ids.size() > AreaTree::kMaxAreas)
                return fail(QStringLiteral("more than %1 areas").arg(AreaTree::kMaxAreas));
            node.area.view = json.value("view").toString();
            if (node.area.view.isEmpty())
                return fail(QStringLiteral("area %1 has no view").arg(node.area.id));
            const QJsonValue state = json.value("state");
            if (!state.isUndefined() && !state.isObject())
                return fail(QStringLiteral("the state of area %1 is not an object").arg(node.area.id));
            node.area.state = state.toObject();
            for (const char *key : {"area", "view", "state"})
                node.extra.remove(QLatin1String(key));
            return true;
        }
        if (type != QLatin1String("branch"))
            return fail(QStringLiteral("a node is neither a leaf nor a branch"));
        node.leaf = false;
        const QString axis = json.value("axis").toString();
        if (axis != QLatin1String("row") && axis != QLatin1String("column"))
            return fail(QStringLiteral("a branch's axis is neither row nor column"));
        node.axis = axis == QLatin1String("row") ? AreaAxis::Row : AreaAxis::Column;
        const QJsonArray children = json.value("children").toArray();
        if (children.isEmpty())
            return fail(QStringLiteral("a branch has no children"));
        for (const QJsonValue &entry : children) {
            const QJsonObject child = entry.toObject();
            const QJsonValue size = child.value("size");
            if (!size.isDouble() || !std::isfinite(size.toDouble()) || size.toDouble() <= 0)
                return fail(QStringLiteral("a size is not a positive number"));
            if (!child.value("node").isObject())
                return fail(QStringLiteral("a branch's child has no node"));
            AreaTree::Node read;
            if (!this->read(child.value("node").toObject(), read, depth + 1))
                return false;
            read.size = size.toDouble();
            node.children.push_back(std::move(read));
        }
        for (const char *key : {"axis", "children"})
            node.extra.remove(QLatin1String(key));
        return true;
    }
};

void place(const AreaTree::Node &node, const QRectF &rect, const AreaTree::MinimumSize &minimum, bool rightToLeft,
           QList<AreaTree::Placed> &into) {
    if (node.leaf) {
        into << AreaTree::Placed{node.area.id, rect};
        return;
    }
    const QList<QRectF> rects = AreaTree::childRects(node, rect, minimum, rightToLeft);
    for (int i = 0; i < int(node.children.size()); ++i)
        place(node.children[size_t(i)], rects.at(i), minimum, rightToLeft, into);
}

} // namespace

AreaTree::AreaTree() : m_root(leafNode(1, graphView)) {}

QList<int> AreaTree::areaIds() const {
    QList<int> ids;
    collectIds(m_root, ids);
    return ids;
}

const Area *AreaTree::area(int id) const {
    bool found = false;
    const Path path = pathOf(id, &found);
    return found ? &node(path)->area : nullptr;
}

AreaTree::Path AreaTree::pathOf(int id, bool *found) const {
    Path path;
    const bool there = findPath(m_root, id, path);
    if (found)
        *found = there;
    return there ? path : Path();
}

const AreaTree::Node *AreaTree::node(const Path &path) const {
    return nodeAt(const_cast<Node &>(m_root), path);
}

int AreaTree::split(int id, AreaAxis axis, const QString &view, double share, bool atEnd) {
    bool found = false;
    const Path path = pathOf(id, &found);
    if (!found || view.isEmpty() || !(share > 0 && share < 1) || count() >= kMaxAreas)
        return 0;
    const QList<int> ids = areaIds();
    const int made = *std::max_element(ids.begin(), ids.end()) + 1;
    Node *leaf = nodeAt(m_root, path);
    Node *parent = path.isEmpty() ? nullptr : nodeAt(m_root, path.mid(0, path.size() - 1));
    Node added = leafNode(made, view);
    const bool sibling = parent && parent->axis == axis;
    if (!sibling && path.size() + 1 > kMaxDepth)
        return 0; // deeper than a saved tree may be (fromJson)
    if (sibling) {
        const int index = path.last();
        added.size = leaf->size * share;
        leaf->size *= 1 - share;
        parent->children.insert(parent->children.begin() + index + (atEnd ? 1 : 0), std::move(added));
    } else {
        Node old = std::move(*leaf);
        Node branch;
        branch.leaf = false;
        branch.axis = axis;
        branch.size = old.size;
        old.size = 1 - share;
        added.size = share;
        if (atEnd) {
            branch.children.push_back(std::move(old));
            branch.children.push_back(std::move(added));
        } else {
            branch.children.push_back(std::move(added));
            branch.children.push_back(std::move(old));
        }
        *leaf = std::move(branch);
    }
    normalize(m_root);
    return made;
}

bool AreaTree::join(int keep, int gone) {
    bool keepFound = false, goneFound = false;
    const Path keepPath = pathOf(keep, &keepFound);
    const Path gonePath = pathOf(gone, &goneFound);
    if (!keepFound || !goneFound || keep == gone || keepPath.size() != gonePath.size() || keepPath.isEmpty())
        return false;
    const Path parentPath = keepPath.mid(0, keepPath.size() - 1);
    if (gonePath.mid(0, gonePath.size() - 1) != parentPath || std::abs(keepPath.last() - gonePath.last()) != 1)
        return false;
    Node *parent = nodeAt(m_root, parentPath);
    parent->children[size_t(keepPath.last())].size += parent->children[size_t(gonePath.last())].size;
    parent->children.erase(parent->children.begin() + gonePath.last());
    normalize(m_root);
    return true;
}

bool AreaTree::resize(const Path &path, int border, double delta, double length, const MinimumSize &minimum) {
    Node *branch = nodeAt(m_root, path);
    if (!branch || branch->leaf || border < 0 || border + 1 >= int(branch->children.size()) || !(length > 0)
        || !std::isfinite(delta))
        return false;
    Node &first = branch->children[size_t(border)];
    Node &second = branch->children[size_t(border + 1)];
    const double a = first.size * length, b = second.size * length;
    const double minA = along(branch->axis, minimumSizeOf(first, minimum));
    const double minB = along(branch->axis, minimumSizeOf(second, minimum));
    if (a + b < minA + minB)
        return false;
    const double moved = std::clamp(a + delta, minA, a + b - minB);
    if (std::abs(moved - a) < 1e-9)
        return false;
    first.size = moved / length;
    second.size = (a + b - moved) / length;
    return true;
}

bool AreaTree::setView(int id, const QString &view) {
    bool found = false;
    const Path path = pathOf(id, &found);
    if (!found || view.isEmpty())
        return false;
    Area &area = nodeAt(m_root, path)->area;
    if (area.view == view)
        return false;
    area.view = view;
    return true;
}

bool AreaTree::setState(int id, const QJsonObject &state) {
    bool found = false;
    const Path path = pathOf(id, &found);
    if (!found)
        return false;
    Area &area = nodeAt(m_root, path)->area;
    area.state.insert(area.view, state);
    return true;
}

QSizeF AreaTree::minimumSizeOf(const Node &node, const MinimumSize &minimum) {
    if (node.leaf)
        return minimum ? minimum(node.area.view) : QSizeF();
    double sum = 0, most = 0;
    for (const auto &child : node.children) {
        const QSizeF size = minimumSizeOf(child, minimum);
        sum += along(node.axis, size);
        most = std::max(most, across(node.axis, size));
    }
    return node.axis == AreaAxis::Row ? QSizeF(sum, most) : QSizeF(most, sum);
}

QSizeF AreaTree::minimumSize(const MinimumSize &minimum) const { return minimumSizeOf(m_root, minimum); }

bool AreaTree::fits(const QSizeF &size, const MinimumSize &minimum) const {
    const QSizeF needed = minimumSize(minimum);
    return needed.width() <= size.width() && needed.height() <= size.height();
}

QList<double> AreaTree::distribute(double length, const QList<double> &shares, const QList<double> &minimums) {
    const int n = int(shares.size());
    QList<double> sizes(n, 0);
    if (n == 0)
        return sizes;
    double minimumTotal = 0;
    for (int i = 0; i < n; ++i)
        minimumTotal += std::max(0.0, minimums.value(i));
    if (minimumTotal >= length && minimumTotal > 0) {
        // Too small for every minimum: each part in proportion to its minimum.
        for (int i = 0; i < n; ++i)
            sizes[i] = length * std::max(0.0, minimums.value(i)) / minimumTotal;
    } else {
        // Parts under their minimum are held at it; the rest of the length goes to the others by their shares,
        // again until none is under.
        QList<bool> held(n, false);
        for (bool again = true; again;) {
            again = false;
            double free = length, freeShares = 0;
            for (int i = 0; i < n; ++i) {
                if (held[i])
                    free -= minimums.value(i);
                else
                    freeShares += shares.value(i);
            }
            for (int i = 0; i < n; ++i) {
                if (held[i]) {
                    sizes[i] = minimums.value(i);
                    continue;
                }
                sizes[i] = freeShares > 0 ? free * shares.value(i) / freeShares : 0;
                if (sizes[i] < minimums.value(i)) {
                    held[i] = true;
                    again = true;
                }
            }
        }
    }
    // Whole-pixel borders: each part ends where the running total rounds to.
    QList<double> whole(n, 0);
    double end = 0, before = 0;
    for (int i = 0; i < n; ++i) {
        end += sizes[i];
        const double at = i == n - 1 ? std::round(length) : std::round(end);
        whole[i] = at - before;
        before = at;
    }
    return whole;
}

QList<QRectF> AreaTree::childRects(const Node &branch, const QRectF &rect, const MinimumSize &minimum, bool rightToLeft) {
    QList<QRectF> rects;
    if (branch.leaf)
        return rects;
    QList<double> shares, minimums;
    for (const auto &child : branch.children) {
        shares << child.size;
        minimums << along(branch.axis, minimumSizeOf(child, minimum));
    }
    const bool row = branch.axis == AreaAxis::Row;
    const QList<double> lengths = distribute(row ? rect.width() : rect.height(), shares, minimums);
    double offset = 0;
    for (const double length : lengths) {
        if (row) {
            const double x = rightToLeft ? rect.right() - offset - length : rect.left() + offset;
            rects << QRectF(x, rect.top(), length, rect.height());
        } else {
            rects << QRectF(rect.left(), rect.top() + offset, rect.width(), length);
        }
        offset += length;
    }
    return rects;
}

QList<AreaTree::Placed> AreaTree::layout(const QRectF &rect, const MinimumSize &minimum, bool rightToLeft) const {
    QList<Placed> placed;
    place(m_root, rect, minimum, rightToLeft, placed);
    return placed;
}

QJsonObject AreaTree::nodeToJson(const Node &node) {
    QJsonObject json = node.extra;
    if (node.leaf) {
        json.insert("type", "leaf");
        json.insert("area", node.area.id);
        json.insert("view", node.area.view);
        json.insert("state", node.area.state);
        return json;
    }
    json.insert("type", "branch");
    json.insert("axis", axisName(node.axis));
    QJsonArray children;
    for (const auto &child : node.children)
        children.append(QJsonObject{{"size", child.size}, {"node", nodeToJson(child)}});
    json.insert("children", children);
    return json;
}

QJsonObject AreaTree::toJson() const {
    QJsonObject json = m_extra;
    json.insert("format", kFormat);
    json.insert("root", nodeToJson(m_root));
    return json;
}

QByteArray AreaTree::save() const { return writeJson(toJson()); }

bool AreaTree::nodeFromJson(const QJsonObject &json, Node *node, QString *problem, bool normalized) {
    Reader reader;
    Node read;
    if (!reader.read(json, read, 0)) {
        if (problem)
            *problem = reader.problem;
        return false;
    }
    if (normalized)
        normalize(read);
    read.size = 1;
    if (node)
        *node = std::move(read);
    return true;
}

AreaTree AreaTree::fromJson(const QJsonObject &json, QString *problem) {
    const auto fail = [problem](const QString &why) {
        if (problem)
            *problem = why;
        return AreaTree();
    };
    if (!isInteger(json.value("format")) || json.value("format").toInt() != kFormat)
        return fail(QStringLiteral("not format %1").arg(kFormat));
    if (!json.value("root").isObject())
        return fail(QStringLiteral("no root"));
    AreaTree tree;
    QString why;
    if (!nodeFromJson(json.value("root").toObject(), &tree.m_root, &why))
        return fail(why);
    tree.m_extra = json;
    tree.m_extra.remove("format");
    tree.m_extra.remove("root");
    if (problem)
        problem->clear();
    return tree;
}

AreaTree AreaTree::load(const QByteArray &bytes, QString *problem) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        if (problem)
            *problem = QStringLiteral("not a JSON object");
        return AreaTree();
    }
    return fromJson(document.object(), problem);
}

} // namespace fugue
