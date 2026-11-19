#include "canvas/canvas_model.h"

#include "engine/node_type.h"
#include "engine/registry.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace fugue;

namespace {

constexpr qreal bottomPadding = 6;
constexpr qreal columnGap = 80;
constexpr qreal stackGap = 40;

// Every link in an input value, with the list item it fills (-1: the whole value).
QList<std::pair<PortRef, int>> linksIn(const InputValue &value) {
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

QString number(qreal value) { return QString::number(value, 'f', 1); }

std::optional<QPointF> point(const QJsonValue &value) {
    const QJsonArray pair = value.toArray();
    if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble())
        return std::nullopt;
    return QPointF(pair[0].toDouble(), pair[1].toDouble());
}

// A horizontal-tangent cubic from p to q, as a node editor draws a wire.
QString cubic(const QPointF &p, const QPointF &q) {
    const qreal dx = std::max<qreal>(40, std::abs(q.x() - p.x()) * 0.5);
    return QStringLiteral(" C %1 %2 %3 %4 %5 %6")
        .arg(number(p.x() + dx), number(p.y()), number(q.x() - dx), number(q.y()), number(q.x()), number(q.y()));
}

} // namespace

// --- the list models ---------------------------------------------------------

NodeList::NodeList(CanvasModel *canvas) : QAbstractListModel(canvas), m_canvas(canvas) {}

int NodeList::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(m_canvas->m_rows.size());
}

QVariant NodeList::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= int(m_canvas->m_rows.size()))
        return {};
    const auto &row = m_canvas->m_rows[size_t(index.row())];
    switch (role) {
    case NodeId: return row.id;
    case TypeId: return m_canvas->m_document.graph.nodes.value(row.id).type;
    case Title: return row.title;
    case Missing: return row.missing;
    case X: return row.pos.x();
    case Y: return row.pos.y();
    case Width: return row.width;
    case Height: return row.height;
    case Collapsed: return row.collapsed;
    case Selected: return m_canvas->m_selected.contains(row.id);
    case Inputs: return m_canvas->portMaps(row, false);
    case Outputs: return m_canvas->portMaps(row, true);
    default: return {};
    }
}

QHash<int, QByteArray> NodeList::roleNames() const {
    return {{NodeId, "nodeId"}, {TypeId, "typeId"},   {Title, "title"},       {Missing, "missing"},
            {X, "posX"},        {Y, "posY"},          {Width, "nodeWidth"},   {Height, "nodeHeight"},
            {Collapsed, "collapsed"}, {Selected, "selected"}, {Inputs, "inputs"}, {Outputs, "outputs"}};
}

LinkList::LinkList(CanvasModel *canvas) : QAbstractListModel(canvas), m_canvas(canvas) {}

int LinkList::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(m_canvas->m_links.size());
}

QVariant LinkList::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= int(m_canvas->m_links.size()))
        return {};
    const auto &link = m_canvas->m_links[size_t(index.row())];
    switch (role) {
    case FromNode: return link.from.node;
    case FromPort: return link.from.port;
    case ToNode: return link.toNode;
    case ToPort: return link.toInput;
    case Item: return link.item;
    case Color: return link.color;
    case X1: return link.a.x();
    case Y1: return link.a.y();
    case X2: return link.b.x();
    case Y2: return link.b.y();
    case Path: return link.path;
    default: return {};
    }
}

QHash<int, QByteArray> LinkList::roleNames() const {
    return {{FromNode, "fromNode"}, {FromPort, "fromPort"}, {ToNode, "toNode"}, {ToPort, "toPort"},
            {Item, "item"},         {Color, "color"},       {X1, "x1"},         {Y1, "y1"},
            {X2, "x2"},             {Y2, "y2"},             {Path, "path"}};
}

// --- the canvas model --------------------------------------------------------

CanvasModel::CanvasModel(const Registry *registry, QObject *parent)
    : QObject(parent), m_registry(registry), m_nodeList(new NodeList(this)), m_linkList(new LinkList(this)) {}

CanvasModel::~CanvasModel() = default;

QString CanvasModel::colorRole(const std::optional<PortSpec> &type, PortKind kind) {
    if (!type)
        return QStringLiteral("muted");
    if (type->type.isAny())
        return QStringLiteral("foreground");
    static const QHash<QString, QString> builtin{
        {"text", "green"}, {"number", "yellow"}, {"boolean", "red"},
        {"image", "blue"}, {"video", "magenta"}, {"audio", "cyan"},
    };
    if (kind == PortKind::Builtin)
        return builtin.value(type->type.id, QStringLiteral("muted"));
    // A declared type: a stable pick by its id (and version), since a pack names its own.
    static const QStringList roles{"orange", "cyan", "magenta", "green", "yellow", "red", "blue"};
    quint32 hash = 2166136261u; // FNV-1a, the same in every run
    for (const QChar c : type->type.toString()) {
        hash ^= c.unicode();
        hash *= 16777619u;
    }
    return roles[int(hash % quint32(roles.size()))];
}

QString CanvasModel::shape(const std::optional<PortSpec> &type, PortKind kind) {
    if (!type)
        return QStringLiteral("circle");
    if (type->type.isAny())
        return QStringLiteral("ring");
    if (kind != PortKind::Builtin)
        return QStringLiteral("diamond");
    static const QSet<QString> media{"image", "video", "audio"};
    return media.contains(type->type.id) ? QStringLiteral("square") : QStringLiteral("circle");
}

void CanvasModel::setDocument(const Document &document) {
    m_nodeList->beginResetModel();
    m_linkList->beginResetModel();
    m_document = document;
    m_rows.clear();
    m_rowOf.clear();
    m_selected.clear();
    m_drag.clear();
    m_dragUnreachable.clear();
    m_moving.clear();
    m_moveStart.clear();

    const QJsonObject layoutNodes = m_document.layout.value("nodes").toObject();
    QSet<QString> unplaced;
    for (auto it = m_document.graph.nodes.cbegin(); it != m_document.graph.nodes.cend(); ++it) {
        NodeRow row = makeRow(it.key());
        if (const auto pos = point(layoutNodes.value(it.key()).toObject().value("pos")))
            row.pos = *pos;
        else
            unplaced.insert(it.key());
        m_rowOf.insert(row.id, int(m_rows.size()));
        m_rows.push_back(std::move(row));
    }
    if (!unplaced.isEmpty())
        autoPlace(unplaced);

    m_view.clear();
    const QJsonObject view = m_document.layout.value("view").toObject();
    if (const auto center = point(view.value("center")); center && view.value("zoom").toDouble() > 0)
        m_view = {{"center", QVariantList{center->x(), center->y()}}, {"zoom", view.value("zoom").toDouble()}};

    m_links.clear();
    rebuildLinks();
    m_nodeList->endResetModel();
    m_linkList->endResetModel();
    emit framesChanged();
    emit linkDragChanged();
    emit documentReset();
}

Document CanvasModel::document() const {
    Document document = m_document;
    QJsonObject layout = document.layout;
    QJsonObject nodes = layout.value("nodes").toObject();
    for (const NodeRow &row : m_rows) {
        QJsonObject entry = nodes.value(row.id).toObject();
        entry.insert("pos", QJsonArray{qRound64(row.pos.x()), qRound64(row.pos.y())});
        nodes.insert(row.id, entry);
    }
    // Entries of nodes that are gone would only confuse a reader.
    for (const QString &id : nodes.keys())
        if (!m_rowOf.contains(id))
            nodes.remove(id);
    if (!nodes.isEmpty())
        layout.insert("nodes", nodes);
    if (!m_view.isEmpty()) {
        const QVariantList center = m_view.value("center").toList();
        layout.insert("view", QJsonObject{{"center", QJsonArray{qRound64(center.value(0).toDouble()),
                                                                 qRound64(center.value(1).toDouble())}},
                                          {"zoom", std::round(m_view.value("zoom").toDouble() * 1000) / 1000}});
    }
    document.layout = layout;
    return document;
}

QVariantList CanvasModel::frames() const {
    QVariantList frames;
    for (const QJsonValue &value : m_document.layout.value("frames").toArray()) {
        const QJsonObject frame = value.toObject();
        const QJsonArray rect = frame.value("rect").toArray();
        if (rect.size() != 4)
            continue;
        frames.append(QVariantMap{{"id", frame.value("id").toString()},
                                  {"title", frame.value("title").toString()},
                                  {"color", frame.value("color").toString()},
                                  {"x", rect[0].toDouble()},
                                  {"y", rect[1].toDouble()},
                                  {"width", rect[2].toDouble()},
                                  {"height", rect[3].toDouble()}});
    }
    return frames;
}

QVariantMap CanvasModel::view() const { return m_view; }

void CanvasModel::setView(qreal centerX, qreal centerY, qreal zoom) {
    m_view = {{"center", QVariantList{centerX, centerY}}, {"zoom", zoom}};
}

void CanvasModel::setHeaderHeight(qreal height) {
    if (height <= 0 || qFuzzyCompare(height, m_headerHeight))
        return;
    m_headerHeight = height;
    for (NodeRow &row : m_rows)
        layOut(row);
    m_nodeList->beginResetModel();
    m_nodeList->endResetModel();
    for (int i = 0; i < int(m_links.size()); ++i)
        refreshLinkEnds(i);
    emit metricsChanged();
}

void CanvasModel::setRowHeight(qreal height) {
    if (height <= 0 || qFuzzyCompare(height, m_rowHeight))
        return;
    m_rowHeight = height;
    for (NodeRow &row : m_rows)
        layOut(row);
    m_nodeList->beginResetModel();
    m_nodeList->endResetModel();
    for (int i = 0; i < int(m_links.size()); ++i)
        refreshLinkEnds(i);
    emit metricsChanged();
}

// --- a node's face -----------------------------------------------------------

CanvasModel::NodeRow CanvasModel::makeRow(const QString &id) const {
    const Node node = m_document.graph.nodes.value(id);
    NodeRow row;
    row.id = id;
    const QJsonObject layout = m_document.layout.value("nodes").toObject().value(id).toObject();
    portsFromType(row, node);

    // Inputs the document sets but the face lacks (an older or newer minor version, or a
    // placeholder), and outputs other nodes link from: kept as ports of unknown type, so
    // every link has its ends and nothing in the file is hidden.
    for (auto it = node.inputs.cbegin(); it != node.inputs.cend(); ++it) {
        if (std::none_of(row.inputs.begin(), row.inputs.end(), [&](const Port &p) { return p.name == it.key(); })) {
            Port port;
            port.name = port.label = it.key();
            port.widget = linksIn(it.value()).isEmpty();
            row.inputs.push_back(port);
        }
    }
    QStringList linkedOutputs;
    for (const Node &other : m_document.graph.nodes)
        for (const InputValue &value : other.inputs)
            for (const auto &[from, item] : linksIn(value))
                if (from.node == id && !linkedOutputs.contains(from.port))
                    linkedOutputs << from.port;
    linkedOutputs.sort();
    for (const QString &name : linkedOutputs) {
        if (std::none_of(row.outputs.begin(), row.outputs.end(), [&](const Port &p) { return p.name == name; })) {
            Port port;
            port.name = port.label = name;
            port.output = true;
            row.outputs.push_back(port);
        }
    }

    const QString title = layout.value("title").toString();
    if (!title.isEmpty())
        row.title = title;
    const QJsonArray size = layout.value("size").toArray();
    if (size.size() == 2 && size[0].toDouble() > 0)
        row.width = size[0].toDouble();
    row.collapsed = layout.value("collapsed").toBool();
    layOut(row);
    return row;
}

void CanvasModel::portsFromType(NodeRow &row, const Node &node) const {
    auto fromInput = [this](const InputSpec &spec, const QString &name, const QString &label) {
        Port port;
        port.name = name;
        port.label = label.isEmpty() ? (spec.label.isEmpty() ? spec.name : spec.label) : label;
        port.type = spec.type;
        port.kind = m_registry ? m_registry->portKind(spec.type.type) : PortKind::Unknown;
        port.widget = !spec.widget().isEmpty();
        port.required = spec.required();
        port.defaultValue = spec.defaultValue;
        return port;
    };
    auto fromOutput = [this](const OutputSpec &spec, const QString &name) {
        Port port;
        port.name = name;
        port.label = spec.label.isEmpty() ? spec.name : spec.label;
        port.output = true;
        port.type = spec.type;
        port.kind = m_registry ? m_registry->portKind(spec.type.type) : PortKind::Unknown;
        return port;
    };

    if (const NodeType *type = m_registry ? m_registry->nodeType(node.type) : nullptr) {
        row.title = type->name;
        for (const InputSpec &spec : type->inputs)
            row.inputs.push_back(fromInput(spec, spec.name, {}));
        for (const OutputSpec &spec : type->outputs)
            row.outputs.push_back(fromOutput(spec, spec.name));
        return;
    }
    if (const GroupDefinition *group = m_document.group(node.type)) {
        // The face of a group instance: its exposed inputs and outputs, typed by the inner
        // ports they lead to. One level deep; a port inside a nested group stays untyped
        // until groups are built (#26).
        row.title = group->name.isEmpty() ? node.type : group->name;
        auto innerType = [&](const QString &innerNode) -> const NodeType * {
            return m_registry ? m_registry->nodeType(group->nodes.value(innerNode).type) : nullptr;
        };
        for (const GroupInput &exposed : group->inputs) {
            const PortRef target = exposed.targets.value(0);
            const NodeType *type = innerType(target.node);
            const InputSpec *spec = type ? type->input(target.port) : nullptr;
            if (spec) {
                row.inputs.push_back(fromInput(*spec, exposed.name, exposed.label));
            } else {
                Port port;
                port.name = port.label = exposed.name;
                row.inputs.push_back(port);
            }
        }
        for (const GroupOutput &exposed : group->outputs) {
            const NodeType *type = innerType(exposed.from.node);
            const OutputSpec *spec = type ? type->output(exposed.from.port) : nullptr;
            if (spec) {
                row.outputs.push_back(fromOutput(*spec, exposed.name));
            } else {
                Port port;
                port.name = port.label = exposed.name;
                port.output = true;
                row.outputs.push_back(port);
            }
        }
        return;
    }
    // Not installed: a placeholder keeping what the document has (3.6).
    row.title = node.type;
    row.missing = true;
}

// Slots first, as in ComfyUI: wire-only inputs down the left and outputs down the right,
// side by side; then one row per value input (a port and its value).
void CanvasModel::layOut(NodeRow &row) const {
    if (row.collapsed) {
        for (Port &port : row.inputs)
            port.y = m_headerHeight / 2;
        for (Port &port : row.outputs)
            port.y = m_headerHeight / 2;
        row.height = m_headerHeight;
        return;
    }
    auto at = [this](int line) { return m_headerHeight + line * m_rowHeight + m_rowHeight / 2; };
    int slotCount = 0;
    for (Port &port : row.inputs)
        if (!port.widget)
            port.y = at(slotCount++);
    int band = std::max(slotCount, int(row.outputs.size()));
    for (int i = 0; i < int(row.outputs.size()); ++i)
        row.outputs[size_t(i)].y = at(i);
    for (Port &port : row.inputs)
        if (port.widget)
            port.y = at(band++);
    row.height = m_headerHeight + band * m_rowHeight + bottomPadding;
}

// Columns by depth: a node sits one column right of the deepest node it reads from.
void CanvasModel::autoPlace(const QSet<QString> &unplaced) {
    QHash<QString, int> depth;
    std::function<int(const QString &, QSet<QString> &)> depthOf = [&](const QString &id, QSet<QString> &visiting) {
        if (depth.contains(id))
            return depth.value(id);
        if (visiting.contains(id))
            return 0; // a cycle: the compiler reports it; here it only needs to end
        visiting.insert(id);
        int deepest = -1;
        for (const InputValue &value : m_document.graph.nodes.value(id).inputs)
            for (const auto &[from, item] : linksIn(value))
                if (unplaced.contains(from.node) && from.node != id)
                    deepest = std::max(deepest, depthOf(from.node, visiting));
        visiting.remove(id);
        depth.insert(id, deepest + 1);
        return deepest + 1;
    };

    QPointF origin(0, 0);
    bool anyPlaced = false;
    for (const NodeRow &row : m_rows) {
        if (unplaced.contains(row.id))
            continue;
        if (!anyPlaced)
            origin = QPointF(row.pos.x(), row.pos.y() + row.height);
        origin = QPointF(std::min(origin.x(), row.pos.x()), std::max(origin.y(), row.pos.y() + row.height));
        anyPlaced = true;
    }
    if (anyPlaced)
        origin.ry() += columnGap;

    QHash<int, qreal> columnBottom;
    for (NodeRow &row : m_rows) { // graph order, so the result is the same every time
        if (!unplaced.contains(row.id))
            continue;
        QSet<QString> visiting;
        const int column = depthOf(row.id, visiting);
        const qreal y = columnBottom.value(column, origin.y());
        row.pos = QPointF(origin.x() + column * (defaultWidth + columnGap), y);
        columnBottom.insert(column, y + row.height + stackGap);
    }
}

QVariantList CanvasModel::portMaps(const NodeRow &row, bool outputs) const {
    QVariantList maps;
    for (const Port &port : outputs ? row.outputs : row.inputs)
        maps.append(portMap(row, port));
    return maps;
}

QVariantMap CanvasModel::portMap(const NodeRow &row, const Port &port) const {
    QVariantMap map{{"name", port.name},
                    {"label", port.label},
                    {"output", port.output},
                    {"type", port.type ? port.type->toString() : QString()},
                    {"list", port.type && port.type->list},
                    {"shape", shape(port.type, port.kind)},
                    {"color", colorRole(port.type, port.kind)},
                    {"y", port.y},
                    {"wired", isWired(row.id, port.name, port.output)},
                    {"widget", port.widget},
                    {"required", port.required}};
    if (port.widget) {
        bool isDefault = false;
        map.insert("value", displayValue(m_document.graph.nodes.value(row.id).inputs.value(port.name), port, &isDefault));
        map.insert("isDefault", isDefault);
    }
    return map;
}

QString CanvasModel::displayValue(const InputValue &value, const Port &port, bool *isDefault) const {
    auto literal = [this](const QJsonValue &json) -> QString {
        if (json.isBool())
            return json.toBool() ? tr("on") : tr("off");
        if (json.isDouble()) {
            const double d = json.toDouble();
            if (std::abs(d) < 9007199254740992.0 && d == std::floor(d))
                return QString::number(qint64(d));
            return QString::number(d, 'g', 6);
        }
        return json.toString().section('\n', 0, 0);
    };
    *isDefault = false;
    if (value.isEmpty()) {
        *isDefault = true;
        return port.defaultValue.isUndefined() ? QString() : literal(port.defaultValue);
    }
    switch (value.kind()) {
    case InputValue::Kind::Literal: return literal(value.literal());
    case InputValue::Kind::Auto: return tr("random");
    case InputValue::Kind::Resource: return value.handle().value("name").toString();
    case InputValue::Kind::List: return QStringLiteral("[%1]").arg(value.items().size());
    case InputValue::Kind::Link: return {};
    }
    return {};
}

bool CanvasModel::isWired(const QString &node, const QString &port, bool output) const {
    for (const int i : m_linksOf.value(node)) {
        const LinkRow &link = m_links[size_t(i)];
        if (output ? (link.from.node == node && link.from.port == port)
                   : (link.toNode == node && link.toInput == port))
            return true;
    }
    return false;
}

const CanvasModel::Port *CanvasModel::findPort(const QString &node, const QString &port, bool output) const {
    const int r = rowOf(node);
    if (r < 0)
        return nullptr;
    for (const Port &p : output ? m_rows[size_t(r)].outputs : m_rows[size_t(r)].inputs)
        if (p.name == port)
            return &p;
    return nullptr;
}

// --- links -------------------------------------------------------------------

void CanvasModel::rebuildLinks() {
    m_links.clear();
    for (auto it = m_document.graph.nodes.cbegin(); it != m_document.graph.nodes.cend(); ++it) {
        for (auto in = it->inputs.cbegin(); in != it->inputs.cend(); ++in) {
            for (const auto &[from, item] : linksIn(in.value())) {
                LinkRow link;
                link.from = from;
                link.toNode = it.key();
                link.toInput = in.key();
                link.item = item;
                if (rowOf(from.node) >= 0) // a link from a node that is not there has no wire
                    m_links.push_back(link);
            }
        }
    }
    m_linksOf.clear();
    for (int i = 0; i < int(m_links.size()); ++i) {
        const LinkRow &link = m_links[size_t(i)];
        m_linksOf[link.from.node].append(i);
        if (link.toNode != link.from.node)
            m_linksOf[link.toNode].append(i);
    }
    for (int i = 0; i < int(m_links.size()); ++i) {
        const LinkRow &link = m_links[size_t(i)];
        const Port *out = findPort(link.from.node, link.from.port, true);
        m_links[size_t(i)].color = out ? colorRole(out->type, out->kind) : QStringLiteral("muted");
        refreshLinkEnds(i);
    }
}

void CanvasModel::refreshLinkEnds(int index) {
    LinkRow &link = m_links[size_t(index)];
    const int from = rowOf(link.from.node), to = rowOf(link.toNode);
    const Port *out = findPort(link.from.node, link.from.port, true);
    const Port *in = findPort(link.toNode, link.toInput, false);
    if (from < 0 || to < 0 || !out || !in)
        return;
    const NodeRow &a = m_rows[size_t(from)];
    const NodeRow &b = m_rows[size_t(to)];
    link.a = a.pos + QPointF(a.width, out->y);
    link.b = b.pos + QPointF(0, in->y);

    // Through the reroute points of layout.links (4.5), keyed by the target.
    const QString key = link.toNode + '.' + link.toInput + (link.item >= 0 ? '#' + QString::number(link.item) : QString());
    QList<QPointF> points{link.a};
    for (const QJsonValue &p : m_document.layout.value("links").toObject().value(key).toObject().value("points").toArray())
        if (const auto at = point(p))
            points.append(*at);
    points.append(link.b);
    QString path = QStringLiteral("M %1 %2").arg(number(link.a.x()), number(link.a.y()));
    for (int i = 1; i < points.size(); ++i)
        path += cubic(points[i - 1], points[i]);
    link.path = path;
}

// Every node `node` reads from, directly or not.
QSet<QString> CanvasModel::upstreamOf(const QString &node) const {
    QSet<QString> seen;
    QList<QString> todo{node};
    while (!todo.isEmpty()) {
        const QString id = todo.takeLast();
        for (const InputValue &value : m_document.graph.nodes.value(id).inputs)
            for (const auto &[from, item] : linksIn(value))
                if (!seen.contains(from.node)) {
                    seen.insert(from.node);
                    todo.append(from.node);
                }
    }
    return seen;
}

// Every node that reads from `node`, directly or not.
QSet<QString> CanvasModel::downstreamOf(const QString &node) const {
    QSet<QString> seen;
    QList<QString> todo{node};
    while (!todo.isEmpty()) {
        const QString id = todo.takeLast();
        for (const int i : m_linksOf.value(id)) {
            const LinkRow &link = m_links[size_t(i)];
            if (link.from.node == id && !seen.contains(link.toNode)) {
                seen.insert(link.toNode);
                todo.append(link.toNode);
            }
        }
    }
    return seen;
}

QString CanvasModel::problem(const QString &fromNode, const QString &output, const QString &toNode,
                             const QString &input, bool useDrag) const {
    const int from = rowOf(fromNode), to = rowOf(toNode);
    if (from < 0 || to < 0)
        return tr("That node is not on the canvas.");
    if (fromNode == toNode)
        return tr("A node cannot feed itself.");
    const Port *out = findPort(fromNode, output, true);
    const Port *in = findPort(toNode, input, false);
    if (!out || !in)
        return tr("That port is not on the node.");
    for (const int r : {from, to})
        if (m_rows[size_t(r)].missing)
            return tr("%1 is not installed, so its ports have no known type.")
                .arg(m_document.graph.nodes.value(m_rows[size_t(r)].id).type);
    if (!out->type || !in->type)
        return tr("The type of this port is not known.");
    switch (compatibility(*out->type, *in->type)) {
    case Compatibility::Compatible: break;
    case Compatibility::DifferentType:
        return tr("%1 cannot feed %2: the types differ, and Fugue does not convert between them.")
            .arg(out->type->type.toString(), in->type->type.toString());
    case Compatibility::ListIntoSingle:
        return tr("A list of %1 cannot feed an input that takes one.").arg(out->type->type.toString());
    }
    // A link into toNode from fromNode closes a cycle when fromNode already reads from toNode.
    const bool cycle = useDrag ? m_dragUnreachable.contains(m_drag.value("output").toBool() ? toNode : fromNode)
                               : upstreamOf(fromNode).contains(toNode);
    if (cycle)
        return tr("This link would make a cycle: %1 already feeds %2.")
            .arg(m_rows[size_t(to)].title, m_rows[size_t(from)].title);
    return {};
}

QString CanvasModel::linkProblem(const QString &fromNode, const QString &output, const QString &toNode,
                                 const QString &input) const {
    return problem(fromNode, output, toNode, input, false);
}

void CanvasModel::insertLink(const LinkRow &link) {
    const int at = int(m_links.size());
    m_linkList->beginInsertRows({}, at, at);
    m_links.push_back(link);
    m_linksOf[link.from.node].append(at);
    if (link.toNode != link.from.node)
        m_linksOf[link.toNode].append(at);
    const Port *out = findPort(link.from.node, link.from.port, true);
    m_links.back().color = out ? colorRole(out->type, out->kind) : QStringLiteral("muted");
    refreshLinkEnds(at);
    m_linkList->endInsertRows();
}

// Removes the link rows into the input (all items with -1), keeping the rest in order.
void CanvasModel::removeLinksInto(const QString &node, const QString &input, int item) {
    for (int i = int(m_links.size()) - 1; i >= 0; --i) {
        const LinkRow &link = m_links[size_t(i)];
        if (link.toNode != node || link.toInput != input || (item >= 0 && link.item != item))
            continue;
        m_linkList->beginRemoveRows({}, i, i);
        m_links.erase(m_links.begin() + i);
        m_linkList->endRemoveRows();
    }
    m_linksOf.clear();
    for (int i = 0; i < int(m_links.size()); ++i) {
        const LinkRow &link = m_links[size_t(i)];
        m_linksOf[link.from.node].append(i);
        if (link.toNode != link.from.node)
            m_linksOf[link.toNode].append(i);
    }
}

bool CanvasModel::connectPorts(const QString &fromNode, const QString &output, const QString &toNode,
                               const QString &input, int item) {
    if (!problem(fromNode, output, toNode, input, false).isEmpty())
        return false;
    const Port *in = findPort(toNode, input, false);
    Node &node = m_document.graph.nodes[toNode];
    const InputValue link = InputValue::link({fromNode, output});
    const InputValue old = node.inputs.value(input);
    QSet<QString> formerSources;
    for (const auto &[from, i] : linksIn(old))
        formerSources.insert(from.node);

    if (in->type->list && !old.isEmpty()) {
        QList<InputValue> items = old.kind() == InputValue::Kind::List ? old.items() : QList<InputValue>{old};
        items.insert(item < 0 || item > items.size() ? items.size() : item, link);
        node.inputs.insert(input, InputValue::list(items));
    } else {
        node.inputs.insert(input, link);
    }

    removeLinksInto(toNode, input, -1);
    for (const auto &[from, i] : linksIn(node.inputs.value(input))) {
        LinkRow row;
        row.from = from;
        row.toNode = toNode;
        row.toInput = input;
        row.item = i;
        if (rowOf(from.node) >= 0)
            insertLink(row);
    }
    formerSources.insert(fromNode);
    for (const QString &id : formerSources)
        refreshPorts(id);
    refreshPorts(toNode);
    emit edited();
    return true;
}

bool CanvasModel::disconnectInput(const QString &nodeId, const QString &input, int item) {
    if (!m_document.graph.nodes.contains(nodeId))
        return false;
    Node &node = m_document.graph.nodes[nodeId];
    const InputValue old = node.inputs.value(input);
    const auto links = linksIn(old);
    if (links.isEmpty())
        return false;
    QSet<QString> sources;
    if (old.kind() == InputValue::Kind::List) {
        QList<InputValue> items = old.items();
        bool removed = false;
        for (int i = int(items.size()) - 1; i >= 0; --i) {
            if (items[i].kind() == InputValue::Kind::Link && (item < 0 || item == i)) {
                sources.insert(items[i].from().node);
                items.removeAt(i);
                removed = true;
            }
        }
        if (!removed)
            return false;
        if (items.isEmpty())
            node.inputs.remove(input);
        else
            node.inputs.insert(input, InputValue::list(items));
    } else {
        if (item >= 0)
            return false;
        sources.insert(old.from().node);
        node.inputs.remove(input);
    }
    removeLinksInto(nodeId, input, -1);
    for (const auto &[from, i] : linksIn(node.inputs.value(input))) {
        LinkRow row;
        row.from = from;
        row.toNode = nodeId;
        row.toInput = input;
        row.item = i;
        if (rowOf(from.node) >= 0)
            insertLink(row);
    }
    for (const QString &id : sources)
        refreshPorts(id);
    refreshPorts(nodeId);
    emit edited();
    return true;
}

void CanvasModel::refreshPorts(const QString &nodeId) {
    const int r = rowOf(nodeId);
    if (r >= 0)
        emitNodeChanged(r, {NodeList::Inputs, NodeList::Outputs});
}

void CanvasModel::emitNodeChanged(int row, const QList<int> &roles) {
    const QModelIndex index = m_nodeList->index(row, 0);
    emit m_nodeList->dataChanged(index, index, roles);
}

void CanvasModel::beginLinkDrag(const QString &node, const QString &port, bool output) {
    const Port *p = findPort(node, port, output);
    if (!p)
        return;
    QString startNode = node, startPort = port;
    bool startOutput = output;
    if (!output && !(p->type && p->type->list)) {
        const InputValue value = m_document.graph.nodes.value(node).inputs.value(port);
        if (value.kind() == InputValue::Kind::Link && findPort(value.from().node, value.from().port, true)) {
            // Pick the link up: the drag goes on from its source.
            startNode = value.from().node;
            startPort = value.from().port;
            startOutput = true;
            disconnectInput(node, port);
        }
    }
    const Port *start = findPort(startNode, startPort, startOutput);
    const NodeRow &row = m_rows[size_t(rowOf(startNode))];
    // From an output of A, a target that A already reads from closes a cycle; from an input
    // of B, a source that already reads from B does.
    m_dragUnreachable = startOutput ? upstreamOf(startNode) : downstreamOf(startNode);
    m_drag = {{"node", startNode},
              {"port", startPort},
              {"output", startOutput},
              {"color", colorRole(start->type, start->kind)},
              {"x", row.pos.x() + (startOutput ? row.width : 0)},
              {"y", row.pos.y() + start->y}};
    emit linkDragChanged();
}

QString CanvasModel::dragProblem(const QString &node, const QString &port, bool output) const {
    if (m_drag.isEmpty())
        return {};
    const bool fromOutput = m_drag.value("output").toBool();
    if (output == fromOutput)
        return fromOutput ? tr("Connect an output to an input.") : tr("Connect an input to an output.");
    const QString dragNode = m_drag.value("node").toString(), dragPort = m_drag.value("port").toString();
    return fromOutput ? problem(dragNode, dragPort, node, port, true) : problem(node, port, dragNode, dragPort, true);
}

bool CanvasModel::endLinkDrag(const QString &node, const QString &port, bool output) {
    if (m_drag.isEmpty())
        return false;
    const bool fromOutput = m_drag.value("output").toBool();
    const QString dragNode = m_drag.value("node").toString(), dragPort = m_drag.value("port").toString();
    m_drag.clear();
    m_dragUnreachable.clear();
    emit linkDragChanged();
    if (node.isEmpty() || output == fromOutput)
        return false;
    return fromOutput ? connectPorts(dragNode, dragPort, node, port) : connectPorts(node, port, dragNode, dragPort);
}

// --- hit tests ---------------------------------------------------------------

QVariantMap CanvasModel::portAt(qreal x, qreal y, qreal radius) const {
    const QPointF at(x, y);
    QVariantMap best;
    qreal bestDistance = radius;
    for (const NodeRow &row : m_rows) {
        if (row.collapsed || !QRectF(row.pos, QSizeF(row.width, row.height)).adjusted(-radius, 0, radius, 0).contains(at))
            continue;
        for (const bool output : {false, true}) {
            for (const Port &port : output ? row.outputs : row.inputs) {
                const QPointF p = row.pos + QPointF(output ? row.width : 0, port.y);
                const qreal d = QLineF(p, at).length();
                if (d <= bestDistance) {
                    bestDistance = d;
                    best = {{"node", row.id}, {"port", port.name}, {"output", output}};
                }
            }
        }
    }
    return best;
}

QString CanvasModel::nodeAt(qreal x, qreal y) const {
    for (auto it = m_rows.rbegin(); it != m_rows.rend(); ++it)
        if (QRectF(it->pos, QSizeF(it->width, it->height)).contains(QPointF(x, y)))
            return it->id;
    return {};
}

QStringList CanvasModel::nodesIn(const QRectF &rect) const {
    QStringList ids;
    const QRectF area = rect.normalized();
    for (const NodeRow &row : m_rows)
        if (area.contains(QRectF(row.pos, QSizeF(row.width, row.height))))
            ids << row.id;
    return ids;
}

// --- nodes -------------------------------------------------------------------

QString CanvasModel::addNode(const QString &typeId, qreal x, qreal y) {
    const NodeType *type = m_registry ? m_registry->nodeType(typeId) : nullptr;
    if (!type)
        return {};
    const QString id = newNodeId(m_document.graph.nodes);
    Node node;
    node.type = type->id;
    node.version = type->version;
    m_document.graph.nodes.insert(id, node);
    NodeRow row = makeRow(id);
    row.pos = QPointF(x, y);
    const int at = int(m_rows.size());
    m_nodeList->beginInsertRows({}, at, at);
    m_rowOf.insert(id, at);
    m_rows.push_back(std::move(row));
    m_nodeList->endInsertRows();
    emit edited();
    return id;
}

void CanvasModel::beginMove(const QString &node) {
    m_moving.clear();
    m_moveStart.clear();
    if (rowOf(node) < 0)
        return;
    const QStringList ids = m_selected.contains(node) ? selectedIds() : QStringList{node};
    for (const QString &id : ids) {
        m_moving << id;
        m_moveStart.push_back(m_rows[size_t(rowOf(id))].pos);
    }
}

void CanvasModel::moveBy(qreal dx, qreal dy) {
    QSet<int> links;
    for (int i = 0; i < m_moving.size(); ++i) {
        const int r = rowOf(m_moving[i]);
        if (r < 0)
            continue;
        m_rows[size_t(r)].pos = m_moveStart[size_t(i)] + QPointF(dx, dy);
        emitNodeChanged(r, {NodeList::X, NodeList::Y});
        for (const int l : m_linksOf.value(m_moving[i]))
            links.insert(l);
    }
    for (const int l : links) {
        refreshLinkEnds(l);
        const QModelIndex index = m_linkList->index(l, 0);
        emit m_linkList->dataChanged(index, index,
                                     {LinkList::X1, LinkList::Y1, LinkList::X2, LinkList::Y2, LinkList::Path});
    }
}

void CanvasModel::endMove() {
    const bool moved = std::any_of(m_moving.begin(), m_moving.end(), [&](const QString &id) {
        const int i = int(m_moving.indexOf(id));
        return m_rows[size_t(rowOf(id))].pos != m_moveStart[size_t(i)];
    });
    m_moving.clear();
    m_moveStart.clear();
    if (moved)
        emit edited();
}

QPointF CanvasModel::position(const QString &node) const {
    const int r = rowOf(node);
    return r < 0 ? QPointF() : m_rows[size_t(r)].pos;
}

void CanvasModel::select(const QStringList &ids, const QString &mode) {
    const QSet<QString> before = m_selected;
    QSet<QString> given;
    for (const QString &id : ids)
        if (rowOf(id) >= 0)
            given.insert(id);
    if (mode == QLatin1String("add"))
        m_selected.unite(given);
    else if (mode == QLatin1String("subtract"))
        m_selected.subtract(given);
    else if (mode == QLatin1String("toggle"))
        for (const QString &id : given)
            m_selected.contains(id) ? void(m_selected.remove(id)) : void(m_selected.insert(id));
    else
        m_selected = given;
    for (const QString &id : (before - m_selected) + (m_selected - before))
        emitNodeChanged(rowOf(id), {NodeList::Selected});
}

QStringList CanvasModel::selectedIds() const {
    QStringList ids;
    for (const NodeRow &row : m_rows)
        if (m_selected.contains(row.id))
            ids << row.id;
    return ids;
}
