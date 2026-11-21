#include "canvas/canvas_model.h"

#include <QRegularExpression>
#include "canvas/input_links.h"
#include "canvas/node_size.h"

#include "engine/compiler.h"
#include "engine/group_library.h"
#include "engine/groups.h"
#include "engine/node_type.h"
#include "engine/registry.h"
#include "search/node_search.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QRandomGenerator>
#include <QScopedValueRollback>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace fugue;
using namespace canvas_detail;

namespace {

constexpr qreal columnGap = 80;
constexpr qreal stackGap = 40;

QString number(qreal value) { return QString::number(value, 'f', 1); }

// A wire's cubic from p to q with horizontal tangents (docs/canvas-look.md 8.1): half the run
// apart, between 6u and 40u; a backward link loops out and back, 12u plus a quarter of the run.
std::array<QPointF, 4> cubic(const QPointF &p, const QPointF &q, qreal u) {
    const qreal run = q.x() - p.x();
    const qreal dx = run >= 6 * u ? std::clamp(run * 0.5, 6 * u, 40 * u) : 12 * u + std::min(std::abs(run), 100 * u) * 0.25;
    return {p, QPointF(p.x() + dx, p.y()), QPointF(q.x() - dx, q.y()), q};
}

QPointF at(const std::array<QPointF, 4> &c, qreal t) {
    const qreal s = 1 - t;
    return s * s * s * c[0] + 3 * s * s * t * c[1] + 3 * s * t * t * c[2] + t * t * t * c[3];
}

qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b) {
    const QPointF ab = b - a;
    const qreal length = QPointF::dotProduct(ab, ab);
    const qreal t = length > 0 ? std::clamp(QPointF::dotProduct(p - a, ab) / length, 0.0, 1.0) : 0.0;
    return QLineF(p, a + t * ab).length();
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
    case Width: return row.shownWidth();
    case Height: return row.height;
    case Collapsed: return row.collapsed;
    case Mode: return nodeModeName(m_canvas->m_document.graph.nodes.value(row.id).mode);
    case Selected: return m_canvas->m_selected.contains(row.id);
    case Inputs: return m_canvas->portMaps(row, false);
    case Outputs: return m_canvas->portMaps(row, true);
    case Color: return row.color;
    case Pinned: return row.pinned;
    case ThumbnailRect: return row.thumbnail;
    case UpdateAvailable: return m_canvas->updateAvailable(row.id);
    case GroupSize: return row.groupSize;
    default: break;
    }
    const CanvasModel::RunInfo run = m_canvas->m_run.value(row.id);
    switch (role) {
    case RunState: return run.state;
    case Progress: return run.progress;
    case Seconds: return run.seconds;
    case Error: return run.error;
    case Warnings: return run.warnings;
    case Thumbnail: return run.thumbnail;
    case ThumbnailInfo: return run.thumbnailInfo;
    case ThumbnailFile: return run.thumbnailFile;
    case ThumbnailText: return run.thumbnailText;
    case ThumbnailDim: return run.thumbnailDim;
    case Compare: return run.compare;
    case CompareView: return m_canvas->isCompare(row.id) ? QVariant(m_canvas->compareView(row.id)) : QVariant(QVariantMap());
    case Notice: return run.notice;
    default: return {};
    }
}

QHash<int, QByteArray> NodeList::roleNames() const {
    return {{NodeId, "nodeId"}, {TypeId, "typeId"},   {Title, "title"},       {Missing, "missing"},
            {X, "posX"},        {Y, "posY"},          {Width, "nodeWidth"},   {Height, "nodeHeight"},
            {Collapsed, "collapsed"}, {Mode, "mode"}, {Selected, "selected"}, {Inputs, "inputs"}, {Outputs, "outputs"},
            {Color, "color"}, {Pinned, "pinned"}, {RunState, "runState"}, {Progress, "progress"},
            {Seconds, "seconds"}, {Error, "error"}, {Warnings, "warnings"}, {Thumbnail, "thumbnail"},
            {ThumbnailInfo, "thumbnailInfo"}, {ThumbnailFile, "thumbnailFile"}, {ThumbnailRect, "thumbnailRect"}, {ThumbnailText, "thumbnailText"}, {ThumbnailDim, "thumbnailDim"},
            {Compare, "compare"}, {CompareView, "compareView"},
            {Notice, "notice"}, {UpdateAvailable, "updateAvailable"}, {GroupSize, "groupSize"}};
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
    case Muted: {
        const auto from = m_canvas->m_document.graph.nodes.constFind(link.from.node);
        return from != m_canvas->m_document.graph.nodes.constEnd() && from->mode == NodeMode::Mute;
    }
    default: return {};
    }
}

QHash<int, QByteArray> LinkList::roleNames() const {
    return {{FromNode, "fromNode"}, {FromPort, "fromPort"}, {ToNode, "toNode"}, {ToPort, "toPort"},
            {Item, "item"},         {Color, "color"},       {X1, "x1"},         {Y1, "y1"},
            {X2, "x2"},             {Y2, "y2"},             {Path, "path"},     {Muted, "muted"}};
}

// --- the canvas model --------------------------------------------------------

CanvasModel::CanvasModel(const Registry *registry, QObject *parent)
    : QObject(parent), m_registry(registry), m_nodeList(new NodeList(this)), m_linkList(new LinkList(this)),
      m_search(new NodeSearch(registry, this, this)), m_widgets(new WidgetEdit(registry, this, this)),
      m_undo(makeUndoStack()) {
    watchUndoStack();
    m_lensNodes = new QSortFilterProxyModel(this);
    m_lensNodes->setSourceModel(m_nodeList);
    m_lensNodes->setFilterRole(NodeList::NodeId);
    setLensNode({});
}

void CanvasModel::setLensNode(const QString &node) {
    // An exact id, or nothing at all (no id holds a newline).
    const QString pattern = node.isEmpty() ? QStringLiteral("\n")
                                           : QRegularExpression::anchoredPattern(QRegularExpression::escape(node));
    m_lensNodes->setFilterRegularExpression(QRegularExpression(pattern));
}

CanvasModel::~CanvasModel() = default;

QString CanvasModel::colorKey(const std::optional<PortSpec> &type, PortKind kind) {
    if (!type)
        return QStringLiteral("unknown");
    if (type->type.isAny())
        return QStringLiteral("any");
    return kind == PortKind::Builtin ? type->type.id : QStringLiteral("declared");
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
    const bool wasInGroup = !m_scope.isEmpty();
    m_scope.clear();
    m_outerViews.clear();
    m_root = Document();
    m_undo->clear();
    emit documentReplaced();
    loadLevel(document);
    if (wasInGroup)
        emit scopeChanged();
}

void CanvasModel::loadLevel(const Document &document, const QVariantMap *view) {
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
    m_moveOpen = m_cloneOpen = m_dragOpen = false;
    m_movingFrames.clear();
    m_frameStart.clear();
    m_editDepth = 0;

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
    if (view) {
        m_view = *view;
    } else {
        const QJsonObject saved = m_document.layout.value("view").toObject();
        if (const auto center = point(saved.value("center")); center && saved.value("zoom").toDouble() > 0)
            m_view = {{"center", QVariantList{center->x(), center->y()}}, {"zoom", saved.value("zoom").toDouble()}};
    }

    m_links.clear();
    rebuildLinks();
    m_nodeList->endResetModel();
    m_linkList->endResetModel();
    emit framesChanged();
    emit reroutesChanged();
    emit linkDragChanged();
    emit selectionChanged();
    emit documentReset();
}

Document CanvasModel::document() const {
    return m_scope.isEmpty() ? scopeDocument() : merged(scopeDocument());
}

Document CanvasModel::scopeDocument() const {
    Document document = m_document;
    QJsonObject layout = document.layout;
    QJsonObject nodes = layout.value("nodes").toObject();
    for (const NodeRow &row : m_rows) {
        QJsonObject entry = nodes.value(row.id).toObject();
        entry.insert("pos", QJsonArray{qRound64(row.pos.x()), qRound64(row.pos.y())});
        // A pinned size is [width, height], 0 for the part left automatic (4.5).
        if (row.sized || row.pinnedHeight > 0) {
            entry.insert("size", QJsonArray{qRound64(row.sized ? row.width : 0), qRound64(row.pinnedHeight)});
        } else {
            const QJsonArray was = entry.value("size").toArray();
            if (was.at(0).toDouble() > 0 || was.at(1).toDouble() > 0)
                entry.remove("size"); // auto again
        }
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

QVariantList CanvasModel::comments() const {
    QVariantList comments;
    for (const QJsonValue &value : m_document.layout.value("comments").toArray()) {
        const QJsonObject comment = value.toObject();
        const auto pos = point(comment.value("pos"));
        if (!pos)
            continue;
        const qreal width = comment.value("size").toArray().at(0).toDouble();
        comments.append(QVariantMap{{"id", comment.value("id").toString()},
                                    {"text", comment.value("text").toString()},
                                    {"x", pos->x()},
                                    {"y", pos->y()},
                                    {"width", width > 0 ? width : 58 * unit()}});
    }
    return comments;
}

QVariantList CanvasModel::reroutes() const {
    QVariantList points;
    for (const LinkRow &link : m_links)
        for (size_t i = 1; i < link.curves.size(); ++i)
            points.append(QVariantMap{{"x", link.curves[i][0].x()}, {"y", link.curves[i][0].y()}, {"color", link.color}});
    return points;
}

QVariantMap CanvasModel::view() const { return m_view; }

void CanvasModel::setView(qreal centerX, qreal centerY, qreal zoom) {
    m_view = {{"center", QVariantList{centerX, centerY}}, {"zoom", zoom}};
}

void CanvasModel::moveView(qreal centerX, qreal centerY, qreal zoom) {
    setView(centerX, centerY, zoom);
    emit viewMoveRequested();
}

void CanvasModel::setTextSize(int size) {
    if (size <= 0 || size == m_textSize)
        return;
    m_nodeList->beginResetModel();
    m_linkList->beginResetModel();
    m_textSize = size;
    const qreal u = size / 3.0;
    m_headerHeight = qRound(7 * u);
    m_rowHeight = qRound(6 * u);
    m_tallRowHeight = qRound(16 * u);
    m_padTop = qRound(u);
    m_padBottom = qRound(2 * u);
    m_defaultWidth = qRound(60 * u);
    for (NodeRow &row : m_rows)
        layOut(row);
    for (int i = 0; i < int(m_links.size()); ++i)
        refreshLinkEnds(i);
    m_nodeList->endResetModel();
    m_linkList->endResetModel();
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
    // every link has its ends and nothing in the file is hidden. Such an input shows its
    // value read-only: with no spec there is nothing to check a typed value against.
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
    row.sized = size.size() == 2 && size[0].toDouble() > 0;
    row.pinnedHeight = size.size() == 2 ? std::max(0.0, size[1].toDouble()) : 0;
    row.width = row.sized ? size[0].toDouble() : m_defaultWidth;
    row.collapsed = layout.value("collapsed").toBool();
    row.color = layout.value("color").toString();
    row.pinned = layout.value("pinned").toBool();
    row.advancedOpen = layout.value("advanced").toBool();
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
        // A resource input is a value row too: unwired, it shows the model picker (2.5). A list
        // keeps its row but has no widget to edit it with (its value shows as "[n]").
        if (!spec.type.list)
            port.widgetType = port.kind == PortKind::Resource ? QStringLiteral("model") : spec.widget();
        port.widget = !port.widgetType.isEmpty() || (spec.type.list && !spec.widget().isEmpty());
        port.required = spec.required();
        port.section = spec.section;
        port.advanced = spec.advanced || spec.section == QLatin1String(kAdvancedSection);
        port.defaultValue = spec.defaultValue;
        port.min = spec.min;
        port.max = spec.max;
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

    if (m_document.group(node.type)) {
        // The face of a group instance (engine/groups.h): its exposed inputs and outputs, typed
        // by the inner ports they lead to through every level, in its order and sections. An
        // exposed port that does not resolve keeps its row, untyped.
        static const Registry none;
        const NodeType face = groupFace(m_document, m_registry ? *m_registry : none, node.type);
        const GroupDefinition *group = m_document.group(node.type);
        row.title = face.name;
        row.groupSize = int(group->nodes.size());
        for (const GroupInput &exposed : group->inputs) {
            if (const InputSpec *spec = face.input(exposed.name)) {
                row.exposed.insert_or_assign(exposed.name, *spec);
                row.inputs.push_back(fromInput(*spec, exposed.name, spec->label));
            } else {
                Port port;
                port.name = port.label = exposed.name;
                port.section = exposed.section;
                row.inputs.push_back(port);
            }
        }
        for (const GroupOutput &exposed : group->outputs) {
            if (const OutputSpec *spec = face.output(exposed.name)) {
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
    if (const NodeType *type = m_registry ? m_registry->nodeType(node.type) : nullptr) {
        row.title = type->name;
        for (const InputSpec &spec : type->inputs)
            row.inputs.push_back(fromInput(spec, spec.name, {}));
        for (const OutputSpec &spec : type->outputs)
            row.outputs.push_back(fromOutput(spec, spec.name));
        return;
    }
    // Not installed: a placeholder keeping what the document has (3.6).
    row.title = node.type;
    row.missing = true;
}

// The width of a node the layout does not pin: 60u, wider for the longest text value a `line`
// input shows (its label and value side by side, the whole value), up to 120u.
qreal CanvasModel::autoWidth(const NodeRow &row, const Node &node) const {
    qreal needed = 0;
    for (const Port &port : row.inputs) {
        if (!port.widget || port.widgetType != QLatin1String("line"))
            continue;
        const InputValue value = node.inputs.value(port.name);
        if (value.kind() != InputValue::Kind::Literal || !value.literal().isString() || value.literal().toString().isEmpty())
            continue;
        needed = std::max(needed, node_size::lineWidgetWidth(port.label, value.literal().toString(), m_textSize));
    }
    return std::clamp(needed, m_defaultWidth, maxWidth());
}

// Outputs first, then inputs in their order, one row each, as in Blender (docs/canvas-look.md
// 4.3): nothing shares a row, so a widget can span the node. An unlinked multiline input is a
// tall row, taller for the lines its text wraps to (up to 40u); its port stays level with a
// normal row's middle. The run's thumbnail goes under. Auto-size (#49): a width the layout
// does not pin fits the longest text value; a pinned height gives what the content does not
// need to the thumbnail, else to the last multiline input.
void CanvasModel::layOut(NodeRow &row) const {
    row.thumbnail = {};
    const Node node = m_document.graph.nodes.value(row.id);
    row.width = row.sized ? std::clamp(row.width, minWidth(), maxWidth()) : autoWidth(row, node);
    if (row.collapsed) {
        // The title plus room for the kind mark, the meta and the padding, at most 60u and
        // never wider than the node open (docs/canvas-look.md 9).
        const qreal u = m_textSize / 3.0;
        QFont font = qGuiApp ? QGuiApplication::font() : QFont();
        font.setPixelSize(m_textSize);
        font.setWeight(QFont::DemiBold);
        const qreal title = QFontMetricsF(font).horizontalAdvance(row.title);
        row.collapsedWidth = std::ceil(std::min({title + 24 * u, 60 * u, row.width}));
        for (Port &port : row.inputs)
            port.y = port.rowTop = m_headerHeight / 2;
        for (Port &port : row.outputs)
            port.y = port.rowTop = m_headerHeight / 2;
        row.height = m_headerHeight;
        return;
    }
    const auto run = m_run.constFind(row.id);
    const bool hasThumbnail = run != m_run.constEnd() && !run->thumbnail.isEmpty();
    const qreal maxTall = qRound(40 * unit());
    const qreal textWidth = row.width - 10 * unit(); // the well's inset and padding, both sides
    bool canGrow = hasThumbnail; // an item takes what a pinned height adds
    // One pass; `extra` is what a pinned height adds to the item that grows.
    auto build = [&](qreal extra) {
        qreal top = m_headerHeight + m_padTop;
        auto place = [&](Port &port, qreal height) {
            port.rowTop = top;
            port.rowHeight = height;
            port.y = top + m_rowHeight / 2;
            top += height;
        };
        for (Port &port : row.outputs)
            place(port, m_rowHeight);
        Port *grows = nullptr;
        QString section;
        for (Port &port : row.inputs) {
            const InputValue value = node.inputs.value(port.name);
            // A section starts with its header row; Advanced stays folded unless open or linked.
            const QString portSection = port.advanced && port.section.isEmpty() ? QString::fromLatin1(kAdvancedSection) : port.section;
            port.headerTop = -1;
            port.header.clear();
            port.hidden = false;
            if (!portSection.isEmpty() && portSection != section) {
                port.headerTop = top;
                port.header = portSection;
                top += m_rowHeight;
            }
            section = portSection;
            if (port.advanced && !row.advancedOpen && linksIn(value).isEmpty()) {
                port.hidden = true;
                port.rowTop = top;
                port.rowHeight = 0;
                port.y = top - m_rowHeight / 2;
                continue;
            }
            const bool tall = port.widgetType == QLatin1String("multiline") && port.widget && linksIn(value).isEmpty();
            qreal height = m_rowHeight;
            if (tall) {
                height = m_tallRowHeight;
                if (value.kind() == InputValue::Kind::Literal && value.literal().isString())
                    height = std::clamp(node_size::multilineHeight(
                                            node_size::wrappedLines(value.literal().toString(), textWidth, m_textSize,
                                                                    int(maxTall / 16) + 1),
                                            m_textSize),
                                        m_tallRowHeight, std::max(maxTall, m_tallRowHeight));
                grows = &port;
                canGrow = true;
            }
            place(port, height);
            if (tall && !hasThumbnail && extra > 0)
                port.rowHeight = height; // the last one takes the extra below
        }
        // A placeholder says so on a row of its own (spec 11.6), and a group how many nodes it
        // holds (docs/canvas-look.md 9).
        if (row.missing || row.groupSize > 0)
            top += m_rowHeight;
        if (hasThumbnail) {
            // 1.5u in from the sides, 1u under the rows, its height from its aspect (0.5 to 1.5).
            const qreal inset = qRound(1.5 * unit());
            const qreal width = row.width - 2 * inset;
            const qreal height = qRound(width * std::clamp(run->thumbnailAspect, 0.5, 1.5));
            row.thumbnail = QRectF(inset, top + m_padTop, width, height + extra);
            top = row.thumbnail.bottom();
        } else if (grows && extra > 0) {
            // The last multiline input is taller; what is below it moves down, section headers too.
            grows->rowHeight += extra;
            bool below = false;
            for (Port &port : row.inputs) {
                if (below) {
                    port.rowTop += extra;
                    port.y += extra;
                    if (port.headerTop >= 0)
                        port.headerTop += extra;
                }
                if (&port == grows)
                    below = true;
            }
            top += extra;
        }
        row.height = top + m_padBottom + (hasThumbnail || grows ? 0 : extra);
    };
    build(0);
    const qreal content = row.height;
    if (row.pinnedHeight <= 0)
        return;
    // Room that nothing takes, over content that has shrunk since (a thumbnail cleared), was
    // only spare: the node is its content's height, the pin kept for when the content is back.
    const bool spareOnly = !canGrow && content < row.pinnedOver - 0.5;
    row.pinnedOver = std::max(row.pinnedOver, content);
    if (row.pinnedHeight > content + 0.5 && !spareOnly)
        build(row.pinnedHeight - content);
}

bool CanvasModel::relayOut(int r, QList<int> roles, bool resized) {
    NodeRow &row = m_rows[size_t(r)];
    const qreal width = row.shownWidth(), height = row.height;
    const QRectF thumbnail = row.thumbnail;
    QList<qreal> ys;
    for (const Port &port : row.inputs)
        ys << port.y;
    for (const Port &port : row.outputs)
        ys << port.y;
    layOut(row);
    QList<qreal> now;
    for (const Port &port : row.inputs)
        now << port.y;
    for (const Port &port : row.outputs)
        now << port.y;
    const bool moved = ys != now;
    const bool wider = width != row.shownWidth();
    if (wider)
        roles << NodeList::Width;
    if (height != row.height)
        roles << NodeList::Height;
    if (thumbnail != row.thumbnail)
        roles << NodeList::ThumbnailRect;
    if (moved)
        roles << NodeList::Inputs << NodeList::Outputs;
    if (resized)
        roles << NodeList::Width << NodeList::Height << NodeList::ThumbnailRect << NodeList::Inputs << NodeList::Outputs;
    if (roles.isEmpty())
        return false;
    emitNodeChanged(r, roles);
    if (moved || wider || resized)
        refreshLinksOf(row.id);
    return true;
}

void CanvasModel::refreshLinksOf(const QString &node) {
    for (const int l : m_linksOf.value(node)) {
        refreshLinkEnds(l);
        const QModelIndex index = m_linkList->index(l, 0);
        emit m_linkList->dataChanged(index, index, {LinkList::X1, LinkList::Y1, LinkList::X2, LinkList::Y2, LinkList::Path});
    }
}

bool CanvasModel::refreshSize(const QString &node) {
    const int r = rowOf(node);
    return r >= 0 && relayOut(r);
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
        row.pos = QPointF(origin.x() + column * (m_defaultWidth + columnGap), y);
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
    // A placeholder's port has no type of its own: it shows the type of what it is linked to
    // (the output feeding it, or an input it feeds), so its mark is that type's shape.
    std::optional<PortSpec> type = port.type;
    PortKind kind = port.kind;
    if (!type) {
        for (const int i : m_linksOf.value(row.id)) {
            const LinkRow &link = m_links[size_t(i)];
            const Port *other = port.output ? (link.from.node == row.id && link.from.port == port.name
                                                   ? findPort(link.toNode, link.toInput, false) : nullptr)
                                            : (link.toNode == row.id && link.toInput == port.name
                                                   ? findPort(link.from.node, link.from.port, true) : nullptr);
            if (other && other->type) {
                type = other->type;
                kind = other->kind;
                break;
            }
        }
    }
    QVariantMap map{{"name", port.name},
                    {"label", port.label},
                    {"output", port.output},
                    {"type", type ? type->toString() : QString()},
                    {"list", type && type->list},
                    {"shape", shape(type, kind)},
                    {"color", colorKey(type, kind)},
                    {"y", port.y},
                    {"rowTop", port.rowTop},
                    {"rowHeight", port.rowHeight},
                    {"wired", isWired(row.id, port.name, port.output)},
                    {"widget", port.widget},
                    {"widgetType", port.widgetType},
                    {"required", port.required},
                    {"hidden", port.hidden}};
    if (port.headerTop >= 0) {
        map.insert("headerTop", port.headerTop);
        map.insert("header", port.header);
        // The Advanced header shows whether it is open and how many it folds.
        if (port.header == QLatin1String(kAdvancedSection)) {
            map.insert("advancedHeader", true);
            map.insert("advancedOpen", row.advancedOpen);
            int folded = 0;
            for (const Port &other : row.inputs)
                folded += other.hidden ? 1 : 0;
            map.insert("folded", folded);
        }
    }
    // Inside a group: the inner port the group's face exposes, under that name (5.4), and what the
    // face shows for it, its label when it has one (#74).
    if (!m_scope.isEmpty()) {
        if (const GroupDefinition *group = m_document.group(scopeGroup())) {
            const auto expose = [&map](const QString &name, const QString &label) {
                map.insert("exposedAs", name);
                map.insert("exposedLabel", label.isEmpty() ? name : label);
            };
            if (port.output) {
                for (const GroupOutput &exposed : group->outputs)
                    if (exposed.from == PortRef{row.id, port.name})
                        expose(exposed.name, exposed.label);
            } else {
                for (const GroupInput &exposed : group->inputs)
                    if (exposed.targets.contains(PortRef{row.id, port.name}))
                        expose(exposed.name, exposed.label);
            }
        }
    }
    const Node &node = m_document.graph.nodes.value(row.id);
    if (port.widget) {
        bool isDefault = false;
        const InputValue value = node.inputs.value(port.name);
        map.insert("value", displayValue(value, port, &isDefault));
        map.insert("isDefault", isDefault);
        // The word shown for an unset input that has no default: not a value to start typing from.
        map.insert("placeholder", isDefault && port.defaultValue.isUndefined() && !map.value("value").toString().isEmpty());
        // The stored value as it is, for the widget (undefined when unset).
        map.insert("literal", value.toJson().toVariant());
        // A slider fills its value's share of its range.
        const QJsonValue number = value.isEmpty() ? port.defaultValue
                                  : value.kind() == InputValue::Kind::Literal ? value.literal() : QJsonValue();
        if (port.min && port.max && *port.max > *port.min && number.isDouble())
            map.insert("share", std::clamp((number.toDouble() - *port.min) / (*port.max - *port.min), 0.0, 1.0));
        // A toggle shows its state as a switch.
        if (number.isBool())
            map.insert("on", number.toBool());
    }
    if (port.output) {
        // Bypassed (3.5), the output takes the first input of its type, a list from a list
        // and one from one, as the engine's bypass does (compiler.cpp).
        for (const Port &input : row.inputs)
            if (port.type && input.type && input.type->type == port.type->type && input.type->list == port.type->list) {
                map.insert("passFrom", input.name);
                break;
            }
        const auto run = m_run.constFind(row.id);
        if (run != m_run.constEnd() && run->values.contains(port.name))
            map.insert("value", run->values.value(port.name).toString());
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
        if (!port.defaultValue.isUndefined())
            return literal(port.defaultValue);
        // An optional number with no default is left to the run: say so, dimmed, not an empty well.
        const QString kind = port.widgetType;
        return kind == QLatin1String("spin") || kind == QLatin1String("slider") || kind == QLatin1String("seed")
                   ? tr("auto") : QString();
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

const InputSpec *CanvasModel::inputSpec(const QString &node, const QString &input) const {
    if (!m_registry || !m_document.graph.nodes.contains(node))
        return nullptr;
    const QString type = m_document.graph.nodes.value(node).type;
    if (const NodeType *installed = m_registry->nodeType(type))
        return installed->input(input);
    // A group instance's input as the group narrows it, kept with the row (valid until the
    // row is made again: callers use it before they edit).
    if (const int r = rowOf(node); r >= 0) {
        const auto &exposed = m_rows[size_t(r)].exposed;
        if (const auto it = exposed.find(input); it != exposed.end())
            return &it->second;
    }
    return nullptr;
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

std::optional<PortSpec> CanvasModel::portType(const QString &node, const QString &port, bool output) const {
    const Port *found = findPort(node, port, output);
    return found ? found->type : std::nullopt;
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
        m_links[size_t(i)].color = out ? colorKey(out->type, out->kind) : QStringLiteral("unknown");
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
    link.a = a.pos + QPointF(a.shownWidth(), out->y);
    link.b = b.pos + QPointF(0, in->y);

    // Through the reroute points of layout.links (4.5), keyed by the target.
    const QString key = link.toNode + '.' + link.toInput + (link.item >= 0 ? '#' + QString::number(link.item) : QString());
    QList<QPointF> points{link.a};
    for (const QJsonValue &p : m_document.layout.value("links").toObject().value(key).toObject().value("points").toArray())
        if (const auto at = point(p))
            points.append(*at);
    points.append(link.b);
    QString path = QStringLiteral("M %1 %2").arg(number(link.a.x()), number(link.a.y()));
    link.curves.clear();
    QPointF low = link.a, high = link.a;
    for (int i = 1; i < points.size(); ++i) {
        const auto c = cubic(points[i - 1], points[i], unit());
        link.curves.push_back(c);
        for (const QPointF &p : c) {
            low = QPointF(std::min(low.x(), p.x()), std::min(low.y(), p.y()));
            high = QPointF(std::max(high.x(), p.x()), std::max(high.y(), p.y()));
        }
        path += QStringLiteral(" C %1 %2 %3 %4 %5 %6")
                    .arg(number(c[1].x()), number(c[1].y()), number(c[2].x()), number(c[2].y()), number(c[3].x()),
                         number(c[3].y()));
    }
    link.bounds = QRectF(low, high); // the control points hold the curve
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
                             const QString &input, bool useDrag, bool styled) const {
    // Styled: a type name bold, any other name escaped.
    auto type = [styled](const QString &name) { return styled ? "<b>" + name.toHtmlEscaped() + "</b>" : name; };
    auto plain = [styled](const QString &name) { return styled ? name.toHtmlEscaped() : name; };
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
                .arg(plain(m_document.graph.nodes.value(m_rows[size_t(r)].id).type));
    if (!out->type || !in->type)
        return tr("The type of this port is not known.");
    switch (compatibility(*out->type, *in->type)) {
    case Compatibility::Compatible: break;
    case Compatibility::DifferentType:
        return tr("%1 cannot feed %2: the types differ, and Fugue does not convert between them.")
            .arg(type(out->type->type.toString()), type(in->type->type.toString()));
    case Compatibility::ListIntoSingle:
        return tr("A list of %1 cannot feed an input that takes one.").arg(type(out->type->type.toString()));
    }
    // A link into toNode from fromNode closes a cycle when fromNode already reads from toNode.
    const bool cycle = useDrag ? m_dragUnreachable.contains(m_drag.value("output").toBool() ? toNode : fromNode)
                               : upstreamOf(fromNode).contains(toNode);
    if (cycle)
        return tr("This link would make a cycle: %1 already feeds %2.")
            .arg(plain(m_rows[size_t(to)].title), plain(m_rows[size_t(from)].title));
    return {};
}

QString CanvasModel::linkProblem(const QString &fromNode, const QString &output, const QString &toNode,
                                 const QString &input) const {
    return problem(fromNode, output, toNode, input, false);
}

bool CanvasModel::connectPorts(const QString &fromNode, const QString &output, const QString &toNode,
                               const QString &input, int item) {
    if (!problem(fromNode, output, toNode, input, false).isEmpty())
        return false;
    const Port *in = findPort(toNode, input, false);
    Document document = this->scopeDocument();
    Node &node = document.graph.nodes[toNode];
    const InputValue link = InputValue::link({fromNode, output});
    const InputValue old = node.inputs.value(input);
    // Into a list input a link adds an item; anything else it replaces, a literal included.
    if (in->type->list && !old.isEmpty()) {
        QList<InputValue> items = old.kind() == InputValue::Kind::List ? old.items() : QList<InputValue>{old};
        items.insert(item < 0 || item > items.size() ? items.size() : item, link);
        node.inputs.insert(input, InputValue::list(items));
    } else {
        node.inputs.insert(input, link);
    }
    commit(document, tr("Connect"));
    return true;
}

bool CanvasModel::disconnectInput(const QString &nodeId, const QString &input, int item) {
    if (!m_document.graph.nodes.contains(nodeId))
        return false;
    const InputValue old = m_document.graph.nodes.value(nodeId).inputs.value(input);
    const auto links = linksIn(old);
    if (std::none_of(links.begin(), links.end(), [&](const auto &link) { return item < 0 || link.second == item; }))
        return false;
    Document document = this->scopeDocument();
    Node &node = document.graph.nodes[nodeId];
    if (old.kind() == InputValue::Kind::List) {
        QList<InputValue> items = old.items();
        for (int i = int(items.size()) - 1; i >= 0; --i)
            if (items[i].kind() == InputValue::Kind::Link && (item < 0 || item == i))
                items.removeAt(i);
        if (items.isEmpty())
            node.inputs.remove(input);
        else
            node.inputs.insert(input, InputValue::list(items));
    } else {
        node.inputs.remove(input);
    }
    commit(document, tr("Disconnect"));
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
    if (!m_drag.isEmpty())
        endLinkDrag();
    // Picking a link up and putting it down is one edit.
    beginEdit();
    m_dragOpen = true;
    QString startNode = node, startPort = port;
    bool startOutput = output;
    if (!output && !(p->type && p->type->list)) {
        const InputValue value = m_document.graph.nodes.value(node).inputs.value(port);
        if (value.kind() == InputValue::Kind::Link && findPort(value.from().node, value.from().port, true)) {
            // Pick the link up: the drag goes on from its source.
            startNode = value.from().node;
            startPort = value.from().port;
            startOutput = true;
            const QScopedValueRollback<bool> own(m_gestureEditing, true);
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
              {"color", colorKey(start->type, start->kind)},
              {"x", row.pos.x() + (startOutput ? row.shownWidth() : 0)},
              {"y", row.pos.y() + start->y}};
    emit linkDragChanged();
}

QString CanvasModel::dragProblem(const QString &node, const QString &port, bool output, bool styled) const {
    if (m_drag.isEmpty())
        return {};
    const bool fromOutput = m_drag.value("output").toBool();
    if (output == fromOutput)
        return fromOutput ? tr("Connect an output to an input.") : tr("Connect an input to an output.");
    // Links being moved (Shift+drag): the output must feed every input they lead to.
    for (const QVariant &target : m_drag.value("targets").toList()) {
        const QVariantMap t = target.toMap();
        if (const QString why = problem(node, port, t.value("node").toString(), t.value("port").toString(), true, styled);
            !why.isEmpty())
            return why;
    }
    const QString dragNode = m_drag.value("node").toString(), dragPort = m_drag.value("port").toString();
    return fromOutput ? problem(dragNode, dragPort, node, port, true, styled)
                      : problem(node, port, dragNode, dragPort, true, styled);
}

QVariantList CanvasModel::dropTargets(const QString &node) const {
    const int r = rowOf(node);
    if (m_drag.isEmpty() || r < 0 || node == m_drag.value("node").toString())
        return {};
    const bool toInputs = m_drag.value("output").toBool();
    const NodeRow &row = m_rows[size_t(r)];
    QVariantList free, taken;
    for (const Port &port : toInputs ? row.inputs : row.outputs) {
        if (!dragProblem(node, port.name, !toInputs).isEmpty())
            continue;
        // An output feeds any number of inputs; an input with a link would lose it.
        const bool open = !toInputs || !isWired(node, port.name, false);
        (open ? free : taken) << QVariantMap{
            {"port", port.name}, {"label", port.label}, {"output", !toInputs}, {"free", open}};
    }
    return free + taken;
}

bool CanvasModel::endLinkDrag(const QString &node, const QString &port, bool output) {
    if (m_drag.isEmpty())
        return false;
    const bool fromOutput = m_drag.value("output").toBool();
    const QString dragNode = m_drag.value("node").toString(), dragPort = m_drag.value("port").toString();
    const QVariantList targets = m_drag.value("targets").toList();
    const bool allowed = !node.isEmpty() && output != fromOutput && dragProblem(node, port, output).isEmpty();
    const QVariantMap source = m_drag.value("source").toMap();
    m_drag.clear();
    m_dragUnreachable.clear();
    emit linkDragChanged();
    const QScopedValueRollback<bool> own(m_gestureEditing, m_dragOpen); // putting it down is the drag's edit
    bool connected = false;
    if (!targets.isEmpty()) {
        // Moving an output's links: each one now comes from the new output.
        if (allowed) {
            const PortRef old{source.value("node").toString(), source.value("port").toString()};
            Document document = this->scopeDocument();
            for (const QVariant &target : targets) {
                const QVariantMap t = target.toMap();
                Node &into = document.graph.nodes[t.value("node").toString()];
                const QString input = t.value("port").toString();
                const InputValue value = into.inputs.value(input);
                const InputValue link = InputValue::link({node, port});
                if (value.kind() == InputValue::Kind::Link && value.from() == old) {
                    into.inputs.insert(input, link);
                } else if (value.kind() == InputValue::Kind::List) {
                    QList<InputValue> items = value.items();
                    for (InputValue &each : items)
                        if (each.kind() == InputValue::Kind::Link && each.from() == old)
                            each = link;
                    into.inputs.insert(input, InputValue::list(items));
                }
            }
            commit(document, tr("Move links"));
            connected = true;
        }
    } else if (allowed) {
        connected = fromOutput ? connectPorts(dragNode, dragPort, node, port) : connectPorts(node, port, dragNode, dragPort);
    }
    if (m_dragOpen) {
        m_dragOpen = false;
        endEdit(connected ? tr("Connect") : tr("Disconnect"));
    }
    return connected;
}

void CanvasModel::beginLinksMove(const QString &node, const QString &output) {
    if (!findPort(node, output, true))
        return;
    QVariantList targets;
    for (const LinkRow &link : m_links)
        if (link.from.node == node && link.from.port == output)
            targets.append(QVariantMap{{"node", link.toNode}, {"port", link.toInput}, {"item", link.item}});
    if (targets.isEmpty()) {
        beginLinkDrag(node, output, true);
        return;
    }
    if (!m_drag.isEmpty())
        endLinkDrag();
    beginEdit();
    m_dragOpen = true;
    // The drag holds the inputs' ends, so it looks for an output, and an output downstream
    // of any of them would close a cycle.
    m_dragUnreachable.clear();
    for (const QVariant &target : targets)
        m_dragUnreachable.unite(downstreamOf(target.toMap().value("node").toString()));
    const QVariantMap first = targets.first().toMap();
    const QString toNode = first.value("node").toString(), toInput = first.value("port").toString();
    const NodeRow &row = m_rows[size_t(rowOf(toNode))];
    const Port *in = findPort(toNode, toInput, false);
    const Port *out = findPort(node, output, true);
    m_drag = {{"node", toNode},
              {"port", toInput},
              {"output", false},
              {"color", colorKey(out->type, out->kind)},
              {"x", row.pos.x()},
              {"y", row.pos.y() + (in ? in->y : 0)},
              {"targets", targets},
              {"source", QVariantMap{{"node", node}, {"port", output}}}};
    emit linkDragChanged();
}

bool CanvasModel::disconnectPort(const QString &nodeId, const QString &port, bool output) {
    if (!findPort(nodeId, port, output))
        return false;
    if (!output)
        return disconnectInput(nodeId, port);
    const PortRef source{nodeId, port};
    Document document = this->scopeDocument();
    bool any = false;
    for (Node &node : document.graph.nodes)
        any |= stripLinks(node, [&](const PortRef &from) { return from == source; });
    if (any)
        commit(document, tr("Disconnect"));
    return any;
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
                const QPointF p = row.pos + QPointF(output ? row.shownWidth() : 0, port.y);
                const qreal d = QLineF(p, at).length();
                if (d <= bestDistance) {
                    bestDistance = d;
                    best = {{"node", row.id}, {"port", port.name}, {"output", output},
                            {"x", p.x()},       {"y", p.y()},          {"shape", shape(port.type, port.kind)}};
                }
            }
        }
    }
    // Filled or hollow: the drag's target ring fits the mark (docs/canvas-look.md 5).
    if (!best.isEmpty())
        best.insert("wired", isWired(best.value("node").toString(), best.value("port").toString(),
                                     best.value("output").toBool()));
    return best;
}

QString CanvasModel::nodeAt(qreal x, qreal y) const {
    for (auto it = m_rows.rbegin(); it != m_rows.rend(); ++it)
        if (QRectF(it->pos, QSizeF(it->shownWidth(), it->height)).contains(QPointF(x, y)))
            return it->id;
    return {};
}

QVariantMap CanvasModel::linkAt(qreal x, qreal y, qreal radius) const {
    const QPointF p(x, y);
    QVariantMap best;
    qreal bestDistance = radius;
    for (int i = 0; i < int(m_links.size()); ++i) {
        const LinkRow &link = m_links[size_t(i)];
        if (link.curves.empty() || !link.bounds.adjusted(-radius, -radius, radius, radius).contains(p))
            continue;
        for (const auto &curve : link.curves) {
            QPointF previous = curve[0];
            for (int step = 1; step <= 16; ++step) {
                const QPointF next = at(curve, step / 16.0);
                const qreal d = distanceToSegment(p, previous, next);
                if (d <= bestDistance) {
                    bestDistance = d;
                    const QPointF mid = at(link.curves[link.curves.size() / 2], link.curves.size() % 2 ? 0.5 : 0.0);
                    best = {{"row", i},          {"fromNode", link.from.node}, {"fromPort", link.from.port},
                            {"toNode", link.toNode}, {"toPort", link.toInput}, {"item", link.item},
                            {"midX", mid.x()},   {"midY", mid.y()}};
                }
                previous = next;
            }
        }
    }
    return best;
}

QStringList CanvasModel::nodesIn(const QRectF &rect) const {
    QStringList ids;
    const QRectF area = rect.normalized();
    for (const NodeRow &row : m_rows)
        if (area.intersects(QRectF(row.pos, QSizeF(row.shownWidth(), row.height))))
            ids << row.id;
    return ids;
}

// --- nodes -------------------------------------------------------------------

namespace {

// A fixed seed a new node starts on: random within the input's bounds and 2^53 - 1 (2.1),
// as WidgetEdit's randomize picks.
double newFixedSeed(const InputSpec &spec) {
    constexpr double maxSeed = 9007199254740991.0;
    const double low = std::max(0.0, spec.min.value_or(0)), high = std::min(maxSeed, spec.max.value_or(maxSeed));
    if (high <= low)
        return low;
    return low + double(QRandomGenerator::global()->bounded(quint64(high - low) + 1));
}

} // namespace

QString CanvasModel::addNode(const QString &typeId, qreal x, qreal y) {
    // A group of the library the workflow does not hold yet: its definition comes in with it,
    // as a paste of one instance does (nested groups that clash get new ids), so the workflow
    // never depends on the library to open (5.3).
    if (const NodeType *type = m_registry ? m_registry->nodeType(typeId) : nullptr;
        type && type->runtime.value("kind") == QLatin1String("group") && !m_document.group(typeId)) {
        const Result<Document> file = loadLibraryGroup(*type);
        if (!file) {
            setGroupProblem(file.error().message);
            return {};
        }
        Document copy = *file;
        Node node;
        node.type = typeId;
        node.version = copy.group(typeId)->version;
        copy.graph.nodes.insert(QStringLiteral("g"), node);
        copy.layout.insert("nodes", QJsonObject{{"g", QJsonObject{{"pos", QJsonArray{0, 0}}}}});
        const QStringList added = insertCopy(copy, QPointF(x, y), false, tr("Add node"), nullptr);
        return added.value(0);
    }
    // A group of this workflow: one more instance of its definition (5.3), unless the level
    // shown is inside it.
    if (const GroupDefinition *group = m_document.group(typeId)) {
        if (!canHoldGroup(m_document, typeId)) {
            setGroupProblem(tr("A group cannot hold itself: %1 holds the group shown.")
                                .arg(group->name.isEmpty() ? typeId : group->name));
            return {};
        }
        Document document = scopeDocument();
        const QString id = newNodeId(document.graph.nodes);
        Node node;
        node.type = typeId;
        node.version = group->version;
        document.graph.nodes.insert(id, node);
        QJsonObject layoutNodes = document.layout.value("nodes").toObject();
        layoutNodes.insert(id, QJsonObject{{"pos", QJsonArray{x, y}}});
        document.layout.insert("nodes", layoutNodes);
        commit(document, tr("Add node"));
        return id;
    }
    const NodeType *type = m_registry ? m_registry->nodeType(typeId) : nullptr;
    if (!type)
        return {};
    Document document = this->scopeDocument();
    const QString id = newNodeId(document.graph.nodes);
    Node node;
    node.type = type->id;
    node.version = type->version;
    // A seed starts on randomize, as ComfyUI's do: each run makes a new picture. The
    // manifest's own default (what the CLI and agents get) stays a number (spec Appendix A).
    // With new_seed "fixed" it starts on a random number that stays.
    for (const InputSpec &input : type->inputs) {
        if (input.widget() == QLatin1String("seed")) {
            node.inputs.insert(input.name, m_newSeed == QLatin1String("fixed")
                                               ? InputValue::literal(newFixedSeed(input))
                                               : InputValue::autoRandom());
        } else if (input.name == QLatin1String("embed") && input.choices && input.choices->contains(m_newEmbed)
                   && QJsonValue(m_newEmbed) != input.defaultValue) {
            node.inputs.insert(input.name, InputValue::literal(m_newEmbed));
        }
    }
    document.graph.nodes.insert(id, node);
    QJsonObject layoutNodes = document.layout.value("nodes").toObject();
    layoutNodes.insert(id, QJsonObject{{"pos", QJsonArray{x, y}}});
    document.layout.insert("nodes", layoutNodes);
    commit(document, tr("Add node"));
    return id;
}

void CanvasModel::setNewNodeOptions(const QString &seed, const QString &embed) {
    m_newSeed = seed;
    m_newEmbed = embed;
}

std::pair<qreal, qreal> CanvasModel::pinnedSize(const QJsonObject &entry) const {
    const QJsonArray size = entry.value("size").toArray();
    const qreal w = size.at(0).toDouble(), h = size.at(1).toDouble();
    return {w > 0 ? std::clamp(w, minWidth(), maxWidth()) : 0, std::max(0.0, h)};
}

// --- resizing (#49) ----------------------------------------------------------------

QVariantMap CanvasModel::resizeCornerAt(qreal x, qreal y, qreal size) const {
    const QString id = nodeAt(x, y);
    if (id.isEmpty())
        return {};
    const NodeRow &row = m_rows[size_t(rowOf(id))];
    if (row.collapsed)
        return {};
    const qreal reach = std::min({size, row.width / 2, row.height / 2});
    const bool west = x < row.pos.x() + reach, east = x >= row.pos.x() + row.width - reach;
    const bool north = y < row.pos.y() + reach, south = y >= row.pos.y() + row.height - reach;
    if (!(west || east) || !(north || south))
        return {};
    return {{"node", id}, {"corner", QString(north ? "n" : "s") + (west ? "w" : "e")}};
}

void CanvasModel::beginResize(const QString &node, const QString &corner) {
    if (m_resizeOpen)
        endResize();
    const int r = rowOf(node);
    if (r < 0)
        return;
    beginEdit();
    m_resizeOpen = true;
    const NodeRow &row = m_rows[size_t(r)];
    m_resizeNode = node;
    m_resizeWest = corner.endsWith('w');
    m_resizeNorth = corner.startsWith('n');
    m_resizeStart = QRectF(row.pos, QSizeF(row.width, row.height));
    m_resizeWasSized = row.sized;
    m_resizeWasWidth = row.width;
    m_resizeWasHeight = row.pinnedHeight;
}

void CanvasModel::resizeBy(qreal dx, qreal dy) {
    const int r = rowOf(m_resizeNode);
    if (!m_resizeOpen || r < 0)
        return;
    NodeRow &row = m_rows[size_t(r)];
    if (dx == 0 && dy == 0) {
        row.sized = m_resizeWasSized;
        row.width = m_resizeWasWidth;
        row.pinnedHeight = m_resizeWasHeight;
    } else {
        row.sized = true;
        row.width = std::round(std::clamp(m_resizeStart.width() + (m_resizeWest ? -dx : dx), minWidth(), maxWidth()));
        row.pinnedHeight = std::round(m_resizeStart.height() + (m_resizeNorth ? -dy : dy));
    }
    row.pinnedOver = 0; // pinned over the content as it is now
    relayOut(r, {}, true);
    if (row.pinnedHeight > 0)
        row.pinnedHeight = std::round(row.height); // never stored below what the content needs
    // The opposite corner stays where it was, whatever size the limits allowed.
    const QPointF pos(m_resizeWest ? m_resizeStart.right() - row.width : m_resizeStart.x(),
                      m_resizeNorth ? m_resizeStart.bottom() - row.height : m_resizeStart.y());
    if (pos != row.pos) {
        row.pos = pos;
        emitNodeChanged(r, {NodeList::X, NodeList::Y});
        refreshLinksOf(row.id);
    }
}

void CanvasModel::endResize() {
    if (!m_resizeOpen)
        return;
    m_resizeOpen = false;
    endEdit(tr("Resize"));
}

bool CanvasModel::autoSize(const QString &node) {
    const int r = rowOf(node);
    if (r < 0 || (!m_rows[size_t(r)].sized && m_rows[size_t(r)].pinnedHeight <= 0))
        return false;
    beginEdit();
    NodeRow &row = m_rows[size_t(r)];
    row.sized = false;
    row.pinnedHeight = 0;
    relayOut(r, {}, true);
    endEdit(tr("Auto size"));
    return true;
}

void CanvasModel::beginMove(const QString &node) {
    if (m_moveOpen)
        endMove();
    m_moving.clear();
    m_moveStart.clear();
    if (rowOf(node) < 0)
        return;
    // The whole drag is one edit, however many steps it takes.
    beginEdit();
    m_moveOpen = true;
    startMoving(m_selected.contains(node) ? selectedIds() : QStringList{node});
}

QStringList CanvasModel::startMoving(const QStringList &ids) {
    QStringList pinned;
    for (const QString &id : ids) {
        if (m_rows[size_t(rowOf(id))].pinned) {
            pinned << id; // pinned in place (#55)
            continue;
        }
        m_moving << id;
        m_moveStart.push_back(m_rows[size_t(rowOf(id))].pos);
    }
    return pinned;
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
    if (!m_movingFrames.isEmpty()) {
        // The Frames a Frame move carries (beginFrameMove) go along, whole units.
        QJsonArray frames = m_document.layout.value("frames").toArray();
        for (qsizetype i = 0; i < frames.size(); ++i) {
            QJsonObject frame = frames.at(i).toObject();
            const qsizetype at = m_movingFrames.indexOf(frame.value("id").toString());
            if (at < 0)
                continue;
            const QRectF rect = m_frameStart[size_t(at)].translated(dx, dy);
            frame.insert("rect", QJsonArray{qRound64(rect.x()), qRound64(rect.y()), qRound64(rect.width()),
                                            qRound64(rect.height())});
            frames.replace(i, frame);
        }
        m_document.layout.insert("frames", frames);
        emit framesChanged();
    }
}

void CanvasModel::endMove() {
    if (!m_movingFrames.isEmpty()) {
        // Each Frame moved holds the moved nodes that are in it now.
        QJsonArray frames = m_document.layout.value("frames").toArray();
        for (qsizetype i = 0; i < frames.size(); ++i) {
            QJsonObject frame = frames.at(i).toObject();
            if (!m_movingFrames.contains(frame.value("id").toString()))
                continue;
            const QJsonArray r = frame.value("rect").toArray();
            const QRectF rect(r.at(0).toDouble(), r.at(1).toDouble(), r.at(2).toDouble(), r.at(3).toDouble());
            QJsonArray members;
            for (const QString &id : m_moving + m_pinnedMembers)
                if (const int row = rowOf(id); row >= 0
                    && rect.contains(QRectF(m_rows[size_t(row)].pos, QSizeF(m_rows[size_t(row)].shownWidth(),
                                                                            m_rows[size_t(row)].height)).center()))
                    members.append(id);
            frame.insert("nodes", members);
            frames.replace(i, frame);
        }
        m_document.layout.insert("frames", frames);
        m_movingFrames.clear();
        m_frameStart.clear();
        m_pinnedMembers.clear();
    }
    m_moving.clear();
    m_moveStart.clear();
    if (m_moveOpen) {
        m_moveOpen = false;
        endEdit(tr("Move"));
    }
    if (m_cloneOpen) {
        m_cloneOpen = false;
        endEdit(tr("Clone"));
    }
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
    const QSet<QString> changed = (before - m_selected) + (m_selected - before);
    for (const QString &id : changed)
        emitNodeChanged(rowOf(id), {NodeList::Selected});
    if (!changed.isEmpty())
        emit selectionChanged();
}

QList<int> CanvasModel::linkRows(const QStringList &nodes) const {
    QList<int> rows;
    for (const QString &id : nodes)
        for (const int row : m_linksOf.value(id))
            if (!rows.contains(row))
                rows << row;
    std::sort(rows.begin(), rows.end());
    return rows;
}

void CanvasModel::setRunState(const QString &node, const QVariantMap &state) {
    const int r = rowOf(node);
    if (r < 0)
        return;
    RunInfo run;
    run.state = state.value("state").toString();
    run.progress = state.value("progress", -1).toReal();
    run.seconds = state.value("seconds", -1).toReal();
    run.error = state.value("error").toString();
    run.warnings = state.value("warnings").toStringList();
    run.thumbnail = state.value("thumbnail").toString();
    run.thumbnailAspect = state.value("thumbnailAspect", 1).toReal();
    run.thumbnailInfo = state.value("thumbnailInfo").toString();
    run.thumbnailFile = state.value("thumbnailFile").toString();
    run.thumbnailSize = state.value("thumbnailSize").toSizeF();
    run.thumbnailText = state.value("thumbnailText").toString();
    run.thumbnailDim = state.value("thumbnailDim").toBool();
    run.compare = state.value("compare").toMap();
    run.values = state.value("values").toMap();
    run.notice = state.value("notice").toString();
    const RunInfo before = m_run.value(node);
    const bool previewChanged = before.thumbnail.isEmpty() != run.thumbnail.isEmpty()
                                || before.thumbnailSize != run.thumbnailSize
                                || before.thumbnailAspect != run.thumbnailAspect;
    m_run.insert(node, run);
    relayOut(r, {NodeList::RunState, NodeList::Progress, NodeList::Seconds, NodeList::Error, NodeList::Warnings,
                 NodeList::Thumbnail, NodeList::ThumbnailInfo, NodeList::ThumbnailFile, NodeList::ThumbnailText, NodeList::ThumbnailDim,
                 NodeList::Compare, NodeList::Outputs, NodeList::Notice});
    if (previewChanged)
        emit previewSizeChanged(node);
}

void CanvasModel::setRunProgress(const QString &node, qreal progress) {
    const int r = rowOf(node);
    const auto run = m_run.find(node);
    if (r < 0 || run == m_run.end() || run->progress == progress)
        return;
    run->progress = progress;
    emitNodeChanged(r, {NodeList::Progress});
}

QRectF CanvasModel::nodeRect(const QString &node) const {
    const int r = rowOf(node);
    if (r < 0)
        return {};
    const NodeRow &row = m_rows[size_t(r)];
    return QRectF(row.pos, QSizeF(row.shownWidth(), row.height));
}

QString CanvasModel::nodeTitle(const QString &node) const {
    const int r = rowOf(node);
    return r < 0 ? QString() : m_rows[size_t(r)].title;
}

bool CanvasModel::isCompare(const QString &node) const {
    return m_document.graph.nodes.value(node).type == QLatin1String("fugue/compare-images");
}

QString CanvasModel::selectedCompare() const {
    return m_selected.size() == 1 && isCompare(*m_selected.constBegin()) ? *m_selected.constBegin() : QString();
}

QVariantMap CanvasModel::compareView(const QString &node) const {
    // The node's widgets say how it starts: a Mode of A or B is the flip mode on that image.
    const fugue::Node &spec = m_document.graph.nodes.value(node);
    const auto literal = [&](const char *name) {
        const fugue::InputValue value = spec.inputs.value(QLatin1String(name));
        return value.kind() == fugue::InputValue::Kind::Literal ? value.literal() : QJsonValue();
    };
    const QString widgetMode = literal("mode").toString(QStringLiteral("slider"));
    const bool flipped = widgetMode == QLatin1String("a") || widgetMode == QLatin1String("b");
    QVariantMap view{{"mode", flipped ? QStringLiteral("flip") : widgetMode},
                     {"position", literal("position").toDouble(0.5)},
                     {"showB", widgetMode == QLatin1String("b")},
                     {"zoom", 1.0}, {"actual", false}, {"centerX", 0.5}, {"centerY", 0.5}};
    const QVariantMap changed = m_compareView.value(node);
    for (auto it = changed.cbegin(); it != changed.cend(); ++it)
        view.insert(it.key(), it.value());
    return view;
}

void CanvasModel::setCompareView(const QString &node, const QVariantMap &changes) {
    const int r = rowOf(node);
    if (r < 0 || !isCompare(node))
        return;
    QVariantMap now = m_compareView.value(node);
    bool differs = false;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        if (compareView(node).value(it.key()) != it.value())
            differs = true;
        now.insert(it.key(), it.value());
    }
    if (!differs)
        return;
    m_compareView.insert(node, now);
    emitNodeChanged(r, {NodeList::CompareView});
    emit compareViewChanged(node);
}

bool CanvasModel::compareStep(const QString &node, const QString &step) {
    if (!isCompare(node))
        return false;
    static const QStringList modes{"slider", "slider_horizontal", "side_by_side", "flip", "difference"};
    const QVariantMap view = compareView(node);
    const QString mode = view.value("mode").toString();
    if (step == QLatin1String("flip")) {
        // From another mode: the flip mode at A; in it, the other image.
        setCompareView(node, mode == QLatin1String("flip") ? QVariantMap{{"showB", !view.value("showB").toBool()}}
                                                            : QVariantMap{{"mode", "flip"}, {"showB", false}});
    } else if (step == QLatin1String("next-mode")) {
        setCompareView(node, {{"mode", modes.value((modes.indexOf(mode) + 1) % modes.size())}});
    } else if (step == QLatin1String("fit")) {
        setCompareView(node, {{"zoom", 1.0}, {"actual", false}, {"centerX", 0.5}, {"centerY", 0.5}});
    } else if (step == QLatin1String("actual")) {
        setCompareView(node, {{"actual", true}});
    } else {
        return false;
    }
    return true;
}

QSizeF CanvasModel::previewSize(const QString &node) const {
    const auto run = m_run.constFind(node);
    return run == m_run.constEnd() || run->thumbnail.isEmpty() ? QSizeF() : run->thumbnailSize;
}

void CanvasModel::clearRunState() {
    const QStringList ids = m_run.keys();
    for (const QString &id : ids)
        setRunState(id, {});
    m_run.clear();
}

QStringList CanvasModel::selectedIds() const {
    QStringList ids;
    for (const NodeRow &row : m_rows)
        if (m_selected.contains(row.id))
            ids << row.id;
    return ids;
}
