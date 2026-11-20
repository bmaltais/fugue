// The canvas model's levels and Frames (spec 5.1, 5.2): entering and leaving a group, the
// breadcrumb, and Frames made, moved, renamed and deleted. canvas_model.h explains the design.

#include "canvas/canvas_model.h"
#include "canvas/input_links.h"

#include "engine/groups.h"
#include "engine/registry.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

using namespace fugue;
using namespace canvas_detail;

namespace {

// The group ids the scope leads through, outside in, or none when it does not resolve.
std::optional<QStringList> chainOf(const Document &root, const QStringList &scope) {
    QStringList chain;
    const Nodes *nodes = &root.graph.nodes;
    for (const QString &id : scope) {
        const auto it = nodes->constFind(id);
        if (it == nodes->constEnd())
            return std::nullopt;
        const GroupDefinition *group = root.group(it->type);
        if (!group)
            return std::nullopt;
        chain << it->type;
        nodes = &group->nodes;
    }
    return chain;
}

QJsonArray rectJson(const QRectF &rect) {
    return QJsonArray{qRound64(rect.x()), qRound64(rect.y()), qRound64(rect.width()), qRound64(rect.height())};
}

std::optional<QRectF> rectOf(const QJsonObject &frame) {
    const QJsonArray rect = frame.value("rect").toArray();
    if (rect.size() != 4)
        return std::nullopt;
    return QRectF(rect[0].toDouble(), rect[1].toDouble(), rect[2].toDouble(), rect[3].toDouble());
}

} // namespace

std::optional<Document> CanvasModel::levelOf(const Document &root, const QStringList &scope) {
    if (scope.isEmpty())
        return root;
    const std::optional<QStringList> chain = chainOf(root, scope);
    if (!chain)
        return std::nullopt;
    const QString group = chain->last();
    Document level = root;
    level.graph.nodes = root.group(group)->nodes;
    QJsonObject layout = root.layout.value("groups").toObject().value(group).toObject();
    layout.remove("view");
    level.layout = layout;
    level.present.layout = true;
    return level;
}

Document CanvasModel::merged(const Document &level) const {
    if (m_scope.isEmpty())
        return level;
    Document root = m_root;
    const QString group = scopeGroup();
    root.groups = level.groups;
    root.present.groups = true;
    root.groups[group].nodes = level.graph.nodes;
    QJsonObject inner = level.layout;
    // A paste inside brings the inner layout of the groups it holds under layout.groups.
    const QJsonObject brought = inner.take("groups").toObject();
    inner.remove("view");
    QJsonObject groupLayouts = root.layout.value("groups").toObject();
    for (auto it = brought.constBegin(); it != brought.constEnd(); ++it)
        if (!groupLayouts.contains(it.key()))
            groupLayouts.insert(it.key(), it.value());
    if (inner.isEmpty())
        groupLayouts.remove(group);
    else
        groupLayouts.insert(group, inner);
    if (groupLayouts.isEmpty())
        root.layout.remove("groups");
    else
        root.layout.insert("groups", groupLayouts);
    if (!root.layout.isEmpty())
        root.present.layout = true;
    // What the face named inside and the level no longer has goes (a deleted inner node).
    static const Registry none;
    pruneGroup(root, group, m_registry ? *m_registry : none);
    return root;
}

QString CanvasModel::scopeGroup() const {
    if (m_scope.isEmpty())
        return {};
    const std::optional<QStringList> chain = chainOf(m_root, m_scope);
    return chain ? chain->last() : QString();
}

QVariantList CanvasModel::breadcrumb() const {
    QVariantList crumbs;
    const Nodes *nodes = &m_root.graph.nodes;
    QJsonObject entries = m_root.layout.value("nodes").toObject();
    for (const QString &id : m_scope) {
        const auto it = nodes->constFind(id);
        const GroupDefinition *group = it == nodes->constEnd() ? nullptr : m_root.group(it->type);
        if (!group)
            break;
        QString title = entries.value(id).toObject().value("title").toString();
        if (title.isEmpty())
            title = group->name.isEmpty() ? it->type : group->name;
        crumbs.append(QVariantMap{{"id", id}, {"title", title}});
        entries = m_root.layout.value("groups").toObject().value(it->type).toObject().value("nodes").toObject();
        nodes = &group->nodes;
    }
    return crumbs;
}

bool CanvasModel::isGroup(const QString &node) const {
    const auto it = m_document.graph.nodes.constFind(node);
    return it != m_document.graph.nodes.constEnd() && m_document.group(it->type);
}

int CanvasModel::groupInstances(const QString &node) const {
    if (!isGroup(node))
        return 0;
    const QString type = m_document.graph.nodes.value(node).type;
    const Document whole = document();
    int count = 0;
    for (const Node &n : whole.graph.nodes)
        count += n.type == type ? 1 : 0;
    for (const GroupDefinition &group : whole.groups)
        for (const Node &n : group.nodes)
            count += n.type == type ? 1 : 0;
    return count;
}

bool CanvasModel::enterGroup(const QString &instance) {
    if (!isGroup(instance))
        return false;
    finishGestures();
    const Document whole = document();
    m_outerViews.push_back(m_view);
    m_root = whole;
    m_scope << instance;
    const QVariantMap none;
    loadLevel(*levelOf(m_root, m_scope), &none);
    emit scopeChanged();
    requestFit();
    return true;
}

bool CanvasModel::leaveGroup() {
    if (m_scope.isEmpty())
        return false;
    finishGestures();
    const Document whole = document();
    const QString left = m_scope.takeLast();
    QVariantMap view;
    if (!m_outerViews.empty()) {
        view = m_outerViews.back();
        m_outerViews.pop_back();
    }
    if (m_scope.isEmpty()) {
        m_root = Document();
        loadLevel(whole, &view);
    } else {
        m_root = whole;
        loadLevel(*levelOf(m_root, m_scope), &view);
    }
    select({left}, QStringLiteral("replace"));
    emit scopeChanged();
    return true;
}

void CanvasModel::leaveTo(int depth) {
    while (m_scope.size() > std::max(0, depth))
        leaveGroup();
}

bool CanvasModel::canHoldGroup(const Document &document, const QString &group) const {
    if (m_scope.isEmpty() || !document.group(group))
        return true;
    const std::optional<QStringList> chain = chainOf(m_root, m_scope);
    if (!chain)
        return true;
    return std::none_of(chain->begin(), chain->end(),
                        [&](const QString &outer) { return groupContains(document, group, outer); });
}

void CanvasModel::setGroupProblem(const QString &problem) {
    if (problem == m_groupProblem)
        return;
    m_groupProblem = problem;
    emit groupProblemChanged();
}

// --- Frames (5.1) ------------------------------------------------------------------

QString CanvasModel::frameSelection() {
    const QStringList ids = selectedIds();
    if (ids.isEmpty())
        return {};
    const qreal u = unit();
    const QRectF rect = boundsOf(ids).adjusted(-4 * u, -12 * u, 4 * u, 4 * u);
    Document document = scopeDocument();
    QJsonArray frames = document.layout.value("frames").toArray();
    QSet<QString> taken;
    for (const QJsonValue &frame : frames)
        taken.insert(frame.toObject().value("id").toString());
    QString id;
    for (int n = 1; id.isEmpty() || taken.contains(id); ++n)
        id = QStringLiteral("f%1").arg(n);
    QJsonArray members;
    for (const QString &node : ids)
        members.append(node);
    frames.append(QJsonObject{{"id", id}, {"title", tr("Frame")}, {"rect", rectJson(rect)}, {"nodes", members}});
    document.layout.insert("frames", frames);
    document.present.layout = true;
    commit(document, tr("Frame"));
    return id;
}

QString CanvasModel::frameAt(qreal x, qreal y, qreal band) const {
    const QJsonArray frames = m_document.layout.value("frames").toArray();
    for (qsizetype i = frames.size() - 1; i >= 0; --i) {
        const QJsonObject frame = frames.at(i).toObject();
        const std::optional<QRectF> rect = rectOf(frame);
        if (rect && rect->contains(QPointF(x, y)) && y <= rect->top() + band)
            return frame.value("id").toString();
    }
    return {};
}

void CanvasModel::beginFrameMove(const QString &frameId) {
    if (m_moveOpen)
        endMove();
    const QJsonArray frames = m_document.layout.value("frames").toArray();
    std::optional<QRectF> area;
    for (const QJsonValue &value : frames)
        if (value.toObject().value("id").toString() == frameId)
            area = rectOf(value.toObject());
    if (!area)
        return;
    // What the Frame holds is what lies in it now: the nodes whose middle is inside, and the
    // Frames inside it.
    QStringList members;
    for (const NodeRow &row : m_rows)
        if (area->contains(QRectF(row.pos, QSizeF(row.shownWidth(), row.height)).center()))
            members << row.id;
    select(members, QStringLiteral("replace"));
    beginEdit();
    m_moveOpen = true;
    m_moving.clear();
    m_moveStart.clear();
    for (const QString &id : members) {
        m_moving << id;
        m_moveStart.push_back(m_rows[size_t(rowOf(id))].pos);
    }
    m_movingFrames.clear();
    m_frameStart.clear();
    for (const QJsonValue &value : frames) {
        const QJsonObject frame = value.toObject();
        const std::optional<QRectF> rect = rectOf(frame);
        if (rect && (frame.value("id").toString() == frameId || area->contains(*rect))) {
            m_movingFrames << frame.value("id").toString();
            m_frameStart.push_back(*rect);
        }
    }
}

void CanvasModel::renameFrame(const QString &frameId, const QString &title) {
    Document document = scopeDocument();
    QJsonArray frames = document.layout.value("frames").toArray();
    for (qsizetype i = 0; i < frames.size(); ++i) {
        QJsonObject frame = frames.at(i).toObject();
        if (frame.value("id").toString() != frameId)
            continue;
        frame.insert("title", title.trimmed());
        frames.replace(i, frame);
    }
    document.layout.insert("frames", frames);
    commit(document, tr("Rename Frame"));
}

void CanvasModel::deleteFrame(const QString &frameId) {
    Document document = scopeDocument();
    QJsonArray frames = document.layout.value("frames").toArray();
    for (qsizetype i = frames.size() - 1; i >= 0; --i)
        if (frames.at(i).toObject().value("id").toString() == frameId)
            frames.removeAt(i);
    if (frames.isEmpty())
        document.layout.remove("frames");
    else
        document.layout.insert("frames", frames);
    commit(document, tr("Delete Frame"));
}
