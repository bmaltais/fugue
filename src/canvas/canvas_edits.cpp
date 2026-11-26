// The canvas model's edits and their undo stack (spec 12.4, 12.11): how a changed document
// becomes the rows on screen, the edit bounds and the undo command, and the edits of the
// selection (delete, bypass, mute, collapse, copy, paste, clone). canvas_model.h explains
// the design.

#include "canvas/align.h"
#include "canvas/canvas_model.h"
#include "canvas/input_links.h"

#include "engine/groups.h"
#include "workflow/content_hash.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QScopedValueRollback>
#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>
#include <limits>

using namespace fugue;
using namespace canvas_detail;

namespace {

// How far Ctrl+D puts a copy from what it copies, and where unplaced nodes in a pasted text go.
constexpr qreal cloneOffset = 20;
constexpr qreal pasteGap = 80;

bool sameNode(const Node &a, const Node &b) {
    return a.type == b.type && a.version == b.version && a.inputs == b.inputs && a.mode == b.mode
           && a.provider == b.provider && a.frozen == b.frozen && a.extra == b.extra;
}

QJsonObject withoutPos(QJsonObject entry) {
    entry.remove("pos");
    return entry;
}

// A layout.links key (4.5): "<node>.<input>", or "<node>.<input>#<item>" for a list item.
struct LinkKey {
    QString node, input;
    int item = -1;
    QString text() const { return node + '.' + input + (item >= 0 ? '#' + QString::number(item) : QString()); }
};

LinkKey parseLinkKey(const QString &key) {
    const QString rest = key.section('.', 1);
    const qsizetype hash = rest.indexOf('#');
    return {key.section('.', 0, 0), hash < 0 ? rest : rest.left(hash), hash < 0 ? -1 : rest.mid(hash + 1).toInt()};
}

// The links into an input, as (source, item) pairs.
QList<std::pair<PortRef, int>> linksInto(const Document &document, const QString &node, const QString &input) {
    const auto it = document.graph.nodes.constFind(node);
    return it == document.graph.nodes.constEnd() ? QList<std::pair<PortRef, int>>() : linksIn(it->inputs.value(input));
}

// A group definition as the file writes it, to tell two apart.
QJsonValue groupJson(const GroupDefinition &group) {
    Document holder;
    holder.groups.insert(QStringLiteral("g"), group);
    holder.present.groups = true;
    return holder.toJson().value("groups").toObject().value("g");
}

// A group's face: its definition without the nodes inside.
QJsonObject faceJson(const GroupDefinition *group) {
    QJsonObject face = group ? groupJson(*group).toObject() : QJsonObject();
    face.remove("nodes");
    return face;
}

// Reroute points belong to their link: after an edit from `before` to `after`, each key of
// after's layout.links names the same link (same source, same input) or goes. A list item's
// points follow it when items before it go; a new link into the same slot gets none.
void followReroutes(const Document &before, Document &after) {
    const QJsonObject was = before.layout.value("links").toObject();
    const QJsonObject now = after.layout.value("links").toObject();
    if (now.isEmpty())
        return;
    QJsonObject kept;
    QSet<QString> taken;
    // Each key's link before the edit, when it had one; then where that link is now.
    auto source = [](const Document &document, const LinkKey &key) -> std::optional<PortRef> {
        for (const auto &[from, item] : linksInto(document, key.node, key.input))
            if (item == key.item)
                return from;
        return std::nullopt;
    };
    for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
        const LinkKey key = parseLinkKey(it.key());
        const std::optional<PortRef> old = was.contains(it.key()) ? source(before, key) : std::nullopt;
        if (!old) {
            // Not there before (a paste brings its own): kept while it names a link.
            if (source(after, key))
                kept.insert(it.key(), it.value());
            continue;
        }
        std::optional<int> at;
        for (const auto &[from, item] : linksInto(after, key.node, key.input)) {
            const QString text = LinkKey{key.node, key.input, item}.text();
            if (!(from == *old) || taken.contains(text))
                continue;
            if (!at || item == key.item)
                at = item;
            if (item == key.item)
                break;
        }
        if (!at)
            continue;
        const QString text = LinkKey{key.node, key.input, *at}.text();
        taken.insert(text);
        kept.insert(text, it.value());
    }
    if (kept == now)
        return;
    if (kept.isEmpty())
        after.layout.remove("links");
    else
        after.layout.insert("links", kept);
}

} // namespace

// One undo step: the document before and after. Pushing it does nothing more, since the edit
// is already made.
class EditCommand : public QUndoCommand {
public:
    EditCommand(CanvasModel *canvas, CanvasModel::Snapshot before, CanvasModel::Snapshot after, const QString &text)
        : QUndoCommand(text), m_canvas(canvas), m_before(std::move(before)), m_after(std::move(after)) {}

    // CanvasModel::rewriteDocuments.
    void rewrite(const std::function<void(Document &)> &change) {
        change(m_before.document);
        change(m_after.document);
    }

    void undo() override { apply(m_before); }
    void redo() override {
        if (m_pushed) {
            m_pushed = false;
            return;
        }
        apply(m_after);
    }

private:
    void apply(const CanvasModel::Snapshot &snapshot) {
        m_canvas->restore(snapshot, true);
        m_canvas->contentChanged();
    }

    CanvasModel *m_canvas;
    CanvasModel::Snapshot m_before, m_after;
    bool m_pushed = true;
};

// --- making the document so ----------------------------------------------------

CanvasModel::Snapshot CanvasModel::snapshot() const {
    QStringList order;
    order.reserve(int(m_rows.size()));
    for (const NodeRow &row : m_rows)
        order << row.id;
    return {document(), order};
}

void CanvasModel::refreshNodeTypes() {
    // Rows hold what they were made from a type, never the type: made again, where they are.
    QSet<QString> all;
    for (int r = 0; r < int(m_rows.size()); ++r) {
        NodeRow &row = m_rows[size_t(r)];
        NodeRow fresh = makeRow(row.id);
        fresh.pos = row.pos;
        row = std::move(fresh);
        all.insert(row.id);
        emitNodeChanged(r, {});
    }
    relink(all);
    m_search->refresh();
}

void CanvasModel::restore(const Snapshot &target, bool selectAdded) {
    // The face of the group shown, from the whole workflow, before and after the edit: the exposed
    // marks inside follow it, so a change refreshes every row of the level (#72).
    const QString level = scopeGroup();
    const QJsonObject faceBefore = level.isEmpty() ? QJsonObject() : faceJson(m_root.group(level));
    // The snapshot is the whole workflow; the canvas shows its level, or the nearest level up
    // that is still there (an undo of the Make group of a group entered since).
    if (!m_scope.isEmpty()) {
        m_root = target.document;
        if (!levelOf(m_root, m_scope)) {
            while (!m_scope.isEmpty() && !levelOf(m_root, m_scope)) {
                m_scope.removeLast();
                if (!m_outerViews.empty()) {
                    m_view = m_outerViews.back();
                    m_outerViews.pop_back();
                }
            }
            const QVariantMap view = m_view;
            loadLevel(m_scope.isEmpty() ? m_root : *levelOf(m_root, m_scope), &view);
            if (m_scope.isEmpty())
                m_root = Document();
            emit scopeChanged();
            return;
        }
    }
    const Document doc = m_scope.isEmpty() ? target.document : *levelOf(m_root, m_scope);
    const Nodes oldNodes = m_document.graph.nodes;
    const QJsonObject oldLayout = m_document.layout;
    const QJsonObject oldEntries = oldLayout.value("nodes").toObject();
    const QJsonObject newEntries = doc.layout.value("nodes").toObject();
    QSet<QString> affected, moved;
    QStringList added;
    bool selectionLost = false;

    // Nodes that are gone, each run of neighbouring rows in one removal (a delete of the
    // whole graph is one change of the list, not one per node).
    for (int r = int(m_rows.size()) - 1; r >= 0; --r) {
        if (doc.graph.nodes.contains(m_rows[size_t(r)].id))
            continue;
        int first = r;
        while (first > 0 && !doc.graph.nodes.contains(m_rows[size_t(first - 1)].id))
            --first;
        for (int i = first; i <= r; ++i) {
            const QString &id = m_rows[size_t(i)].id;
            affected.insert(id);
            if (m_selected.remove(id))
                selectionLost = true;
        }
        m_nodeList->beginRemoveRows({}, first, r);
        m_rows.erase(m_rows.begin() + first, m_rows.begin() + r + 1);
        m_rowOf.clear();
        for (int i = 0; i < int(m_rows.size()); ++i)
            m_rowOf.insert(m_rows[size_t(i)].id, i);
        m_nodeList->endRemoveRows();
        r = first;
    }

    // Group definitions that changed (a face edit, an edit inside): their instances show a new
    // face, and inside a group the exposed marks follow its definition.
    QSet<QString> changedGroups;
    for (auto it = doc.groups.cbegin(); it != doc.groups.cend(); ++it)
        if (!m_document.groups.contains(it.key()) || groupJson(m_document.groups.value(it.key())) != groupJson(it.value()))
            changedGroups.insert(it.key());
    for (auto it = m_document.groups.cbegin(); it != m_document.groups.cend(); ++it)
        if (!doc.groups.contains(it.key()))
            changedGroups.insert(it.key());
    // Inside a group its nodes change with every edit; the marks only follow its face.
    const bool levelChanged = !level.isEmpty() && faceBefore != faceJson(m_root.group(level));
    changedGroups.remove(level);
    auto faceChanged = [&](const QString &type) {
        if (levelChanged)
            return true;
        for (const QString &group : changedGroups)
            if (groupContains(doc, type, group) || groupContains(m_document, type, group))
                return true;
        return false;
    };

    // The viewport is not part of an edit: keep the one on screen.
    m_document = doc;

    // Nodes that changed are made again; nodes that only moved, moved.
    for (int r = 0; r < int(m_rows.size()); ++r) {
        NodeRow &row = m_rows[size_t(r)];
        const QPointF pos = point(newEntries.value(row.id).toObject().value("pos")).value_or(row.pos);
        if (!sameNode(oldNodes.value(row.id), doc.graph.nodes.value(row.id))
            || ((levelChanged || !changedGroups.isEmpty()) && faceChanged(doc.graph.nodes.value(row.id).type))
            || withoutPos(oldEntries.value(row.id).toObject()) != withoutPos(newEntries.value(row.id).toObject())
            || pinnedSize(newEntries.value(row.id).toObject())
                   != std::pair<qreal, qreal>{row.sized ? row.width : 0, row.pinnedHeight}) {
            NodeRow fresh = makeRow(row.id);
            fresh.pos = pos;
            row = std::move(fresh);
            affected.insert(row.id);
            emitNodeChanged(r, {});
        } else if (row.pos != pos) {
            row.pos = pos;
            moved.insert(row.id);
            emitNodeChanged(r, {NodeList::X, NodeList::Y});
        }
    }

    // New nodes, where the drawing order puts them.
    QStringList order = target.order;
    for (auto it = doc.graph.nodes.cbegin(); it != doc.graph.nodes.cend(); ++it)
        if (!order.contains(it.key()))
            order << it.key();
    // Each run of new nodes goes in with one insertion, so an undone delete of a whole graph
    // is one change of the list, not one per node.
    int present = 0; // rows before the run
    std::vector<NodeRow> run;
    auto flush = [&] {
        if (run.empty())
            return;
        const int count = int(run.size());
        m_nodeList->beginInsertRows({}, present, present + count - 1);
        m_rows.insert(m_rows.begin() + present, std::make_move_iterator(run.begin()), std::make_move_iterator(run.end()));
        m_rowOf.clear();
        for (int i = 0; i < int(m_rows.size()); ++i)
            m_rowOf.insert(m_rows[size_t(i)].id, i);
        m_nodeList->endInsertRows();
        present += count;
        run.clear();
    };
    for (const QString &id : order) {
        if (!doc.graph.nodes.contains(id))
            continue;
        if (rowOf(id) >= 0) {
            flush();
            ++present;
            continue;
        }
        NodeRow row = makeRow(id);
        row.pos = point(newEntries.value(id).toObject().value("pos")).value_or(QPointF());
        run.push_back(std::move(row));
        affected.insert(id);
        added << id;
    }
    flush();

    relink(affected);
    // A face that reads what its source carries (the Sampler's Soft mask) follows a change upstream.
    for (int r = 0; r < int(m_rows.size()); ++r)
        if (m_rows[size_t(r)].readsUpstream && !affected.contains(m_rows[size_t(r)].id))
            relayOut(r);

    const bool reroutesChanged = oldLayout.value("links") != doc.layout.value("links");
    QSet<int> ends;
    for (int i = 0; i < int(m_links.size()); ++i) {
        const LinkRow &link = m_links[size_t(i)];
        if (reroutesChanged || moved.contains(link.from.node) || moved.contains(link.toNode))
            ends.insert(i);
    }
    for (const int i : ends) {
        refreshLinkEnds(i);
        const QModelIndex index = m_linkList->index(i, 0);
        emit m_linkList->dataChanged(index, index,
                                     {LinkList::X1, LinkList::Y1, LinkList::X2, LinkList::Y2, LinkList::Path});
    }
    if (reroutesChanged)
        emit this->reroutesChanged();
    if (oldLayout.value("frames") != doc.layout.value("frames") || oldLayout.value("comments") != doc.layout.value("comments"))
        emit framesChanged();
    if (selectAdded && !added.isEmpty())
        select(added, QStringLiteral("replace"));
    else if (selectionLost)
        emit selectionChanged();
}

void CanvasModel::reindexLinks() {
    m_linksOf.clear();
    for (int i = 0; i < int(m_links.size()); ++i) {
        const LinkRow &link = m_links[size_t(i)];
        m_linksOf[link.from.node].append(i);
        if (link.toNode != link.from.node)
            m_linksOf[link.toNode].append(i);
    }
}

// Remakes the link rows into and out of these nodes; ports at either end show their new state.
void CanvasModel::relink(const QSet<QString> &nodes) {
    if (nodes.isEmpty())
        return;
    QSet<QString> ends;
    bool reroutes = false; // a link with reroute points went or came: the points change
    auto touched = [&](const LinkRow &link) { return nodes.contains(link.toNode) || nodes.contains(link.from.node); };
    // Each run of neighbouring rows goes in one removal.
    for (int i = int(m_links.size()) - 1; i >= 0; --i) {
        if (!touched(m_links[size_t(i)]))
            continue;
        int first = i;
        while (first > 0 && touched(m_links[size_t(first - 1)]))
            --first;
        for (int j = first; j <= i; ++j) {
            const LinkRow &link = m_links[size_t(j)];
            ends << link.toNode << link.from.node;
            reroutes |= link.curves.size() > 1;
        }
        m_linkList->beginRemoveRows({}, first, i);
        m_links.erase(m_links.begin() + first, m_links.begin() + i + 1);
        reindexLinks(); // before the signal: a listener may look a link up by node (isWired)
        m_linkList->endRemoveRows();
        i = first;
    }
    reindexLinks();
    // The new rows go in with one insertion at the end.
    std::vector<LinkRow> fresh;
    for (auto it = m_document.graph.nodes.cbegin(); it != m_document.graph.nodes.cend(); ++it) {
        for (auto in = it->inputs.cbegin(); in != it->inputs.cend(); ++in) {
            for (const auto &[from, item] : linksIn(in.value())) {
                if (!nodes.contains(it.key()) && !nodes.contains(from.node))
                    continue;
                if (rowOf(from.node) < 0 || rowOf(it.key()) < 0)
                    continue; // a link from a node that is not there has no wire
                LinkRow row;
                row.from = from;
                row.toNode = it.key();
                row.toInput = in.key();
                row.item = item;
                const Port *out = findPort(from.node, from.port, true);
                row.color = out ? portColor(from.node, *out) : QStringLiteral("unknown");
                fresh.push_back(row);
                ends << from.node << it.key();
            }
        }
    }
    if (!fresh.empty()) {
        const int first = int(m_links.size());
        m_linkList->beginInsertRows({}, first, first + int(fresh.size()) - 1);
        for (LinkRow &row : fresh) {
            const int at = int(m_links.size());
            m_links.push_back(std::move(row));
            const LinkRow &link = m_links.back();
            m_linksOf[link.from.node].append(at);
            if (link.toNode != link.from.node)
                m_linksOf[link.toNode].append(at);
            refreshLinkEnds(at);
            reroutes |= link.curves.size() > 1;
        }
        m_linkList->endInsertRows();
    }
    // A wire out of a node that takes its type from its sides (a Switch) changes color when a side upstream does, also when
    // that node was not itself touched (a Switch that reads another).
    for (int i = 0; i < int(m_links.size()); ++i) {
        LinkRow &link = m_links[size_t(i)];
        const Port *out = findPort(link.from.node, link.from.port, true);
        if (!out || !out->type || !out->type->type.isAny())
            continue;
        const QString color = portColor(link.from.node, *out);
        if (color == link.color)
            continue;
        link.color = color;
        const QModelIndex index = m_linkList->index(i, 0);
        emit m_linkList->dataChanged(index, index, {LinkList::Color});
    }
    for (const QString &id : ends)
        refreshPorts(id);
    if (reroutes)
        emit reroutesChanged();
}

// --- edit bounds and the undo stack ---------------------------------------------

void CanvasModel::beginEdit() {
    // A widget being typed into (#48) ends its step first, so this edit is a step of its own.
    if (m_widgets->editing() && !m_widgets->writing())
        m_widgets->endSession();
    // So does a drag (a key during it, a run's seeds), unless it is the drag's own edit.
    if (!m_gestureEditing && (m_moveOpen || m_cloneOpen || m_resizeOpen || m_dragOpen))
        endMouseGestures();
    if (m_editDepth++ == 0)
        m_editBefore = snapshot();
}

void CanvasModel::endEdit(const QString &text) {
    if (m_editDepth == 0 || --m_editDepth > 0)
        return;
    Snapshot after = snapshot();
    // "Changed" is what "unsaved" means (workflow/content_hash.h): the viewport is not content.
    if (contentHash(m_editBefore.document) == contentHash(after.document))
        return;
    m_undo->push(new EditCommand(this, std::move(m_editBefore), std::move(after), text));
    m_editBefore = {};
    contentChanged();
}

void CanvasModel::commit(const Document &document, const QString &text) {
    beginEdit();
    QStringList order;
    for (const NodeRow &row : m_rows)
        order << row.id;
    Document made = document;
    followReroutes(m_document, made);
    restore({m_scope.isEmpty() ? made : merged(made), order}, false);
    endEdit(text);
}

void CanvasModel::commitWhole(const Document &whole, const QString &text) {
    beginEdit();
    QStringList order;
    for (const NodeRow &row : m_rows)
        order << row.id;
    restore({whole, order}, false);
    endEdit(text);
}

void CanvasModel::rewriteDocuments(const std::function<void(Document &)> &rewrite) {
    // QUndoStack hands its commands out as const; every one it holds here is an EditCommand.
    for (int i = 0; i < m_undo->count(); ++i)
        if (auto *step = dynamic_cast<const EditCommand *>(m_undo->command(i)))
            const_cast<EditCommand *>(step)->rewrite(rewrite);
    Document document = this->document();
    rewrite(document);
    QStringList order;
    for (const NodeRow &row : m_rows)
        order << row.id;
    restore({document, order}, false);
}

void CanvasModel::endMouseGestures() {
    if (!m_drag.isEmpty()) {
        endLinkDrag();
    } else if (m_dragOpen) {
        m_dragOpen = false;
        endEdit(tr("Disconnect"));
    }
    if (m_resizeOpen)
        endResize();
    if (m_moveOpen || m_cloneOpen)
        endMove();
}

void CanvasModel::finishGestures() {
    m_widgets->endSession();
    endMouseGestures();
    while (m_editDepth > 0)
        endEdit(tr("Edit"));
}

void CanvasModel::contentChanged() { emit edited(); }

std::unique_ptr<QUndoStack> CanvasModel::makeUndoStack() {
    auto stack = std::make_unique<QUndoStack>();
    // ComfyUI's change tracker keeps 50 steps too; each step holds two documents.
    stack->setUndoLimit(undoLimit);
    return stack;
}

void CanvasModel::watchUndoStack() {
    connect(m_undo.get(), &QUndoStack::indexChanged, this, &CanvasModel::undoChanged);
    connect(m_undo.get(), &QUndoStack::canUndoChanged, this, &CanvasModel::undoChanged);
    connect(m_undo.get(), &QUndoStack::canRedoChanged, this, &CanvasModel::undoChanged);
}

std::unique_ptr<QUndoStack> CanvasModel::takeUndoStack() {
    finishGestures();
    std::unique_ptr<QUndoStack> taken = std::move(m_undo);
    taken->disconnect(this);
    m_undo = makeUndoStack();
    watchUndoStack();
    emit undoChanged();
    return taken;
}

void CanvasModel::setUndoStack(std::unique_ptr<QUndoStack> stack) {
    if (!stack)
        return;
    finishGestures();
    m_undo = std::move(stack);
    watchUndoStack();
    emit undoChanged();
}

bool CanvasModel::canUndo() const { return m_undo->canUndo(); }
bool CanvasModel::canRedo() const { return m_undo->canRedo(); }
QString CanvasModel::undoText() const { return m_undo->undoText(); }
QString CanvasModel::redoText() const { return m_undo->redoText(); }

void CanvasModel::undo() {
    finishGestures();
    m_undo->undo();
}

void CanvasModel::redo() {
    finishGestures();
    m_undo->redo();
}

// --- edits of the selection ------------------------------------------------------

void CanvasModel::selectAll() {
    QStringList ids;
    for (const NodeRow &row : m_rows)
        ids << row.id;
    select(ids, QStringLiteral("replace"));
}

void CanvasModel::deleteSelection() {
    const QStringList ids = selectedIds();
    if (ids.isEmpty())
        return;
    const QSet<QString> gone(ids.begin(), ids.end());
    Document document = this->scopeDocument();
    for (const QString &id : ids)
        document.graph.nodes.remove(id);
    for (Node &node : document.graph.nodes)
        stripLinks(node, [&](const PortRef &from) { return gone.contains(from.node); });

    // Their layout goes too: entries and places in Frames (commit drops the reroute points
    // of the links that went).
    QJsonObject layout = document.layout;
    QJsonObject entries = layout.value("nodes").toObject();
    for (const QString &id : ids)
        entries.remove(id);
    // None left: no empty entry either, so a workflow that gets a node and loses it again is
    // as it was, not unsaved.
    if (entries.isEmpty())
        layout.remove("nodes");
    else
        layout.insert("nodes", entries);
    if (layout.contains("frames")) {
        QJsonArray frames;
        for (const QJsonValue &value : layout.value("frames").toArray()) {
            QJsonObject frame = value.toObject();
            if (frame.contains("nodes")) {
                QJsonArray kept;
                for (const QJsonValue &id : frame.value("nodes").toArray())
                    if (!gone.contains(id.toString()))
                        kept.append(id);
                frame.insert("nodes", kept);
            }
            frames.append(frame);
        }
        layout.insert("frames", frames);
    }
    document.layout = layout;
    commit(document, tr("Delete"));
}

void CanvasModel::toggleMode(const QString &modeName) {
    const std::optional<NodeMode> mode = parseNodeMode(modeName);
    const QStringList ids = selectedIds();
    if (!mode || *mode == NodeMode::Normal || ids.isEmpty())
        return;
    Document document = this->scopeDocument();
    const bool all = std::all_of(ids.begin(), ids.end(),
                                 [&](const QString &id) { return document.graph.nodes.value(id).mode == *mode; });
    for (const QString &id : ids)
        document.graph.nodes[id].mode = all ? NodeMode::Normal : *mode;
    commit(document, *mode == NodeMode::Bypass ? tr("Bypass") : tr("Mute"));
}

bool CanvasModel::selectionAll(const QString &state) const {
    const QStringList ids = selectedIds();
    if (ids.isEmpty())
        return false;
    return std::all_of(ids.begin(), ids.end(), [&](const QString &id) {
        const int r = rowOf(id);
        if (r < 0)
            return false;
        const NodeRow &row = m_rows[size_t(r)];
        if (state == QLatin1String("collapsed"))
            return row.collapsed;
        if (state == QLatin1String("pinned"))
            return row.pinned;
        const std::optional<NodeMode> mode = parseNodeMode(state);
        return mode && m_document.graph.nodes.value(id).mode == *mode;
    });
}

void CanvasModel::toggleCollapsed() { toggleLayoutFlag(QStringLiteral("collapsed"), tr("Collapse")); }

void CanvasModel::togglePinned() { toggleLayoutFlag(QStringLiteral("pinned"), tr("Pin")); }

bool CanvasModel::alignSelection(const QString &mode) {
    QList<align::Box> boxes;
    for (const QString &id : selectedIds())
        if (const int r = rowOf(id); r >= 0)
            boxes.append({id, nodeRect(id), m_rows[size_t(r)].pinned});
    const QHash<QString, QPointF> moves = align::arrange(boxes, mode);
    if (moves.isEmpty())
        return false;
    Document document = this->scopeDocument();
    QJsonObject entries = document.layout.value("nodes").toObject();
    for (auto it = moves.cbegin(); it != moves.cend(); ++it) {
        QJsonObject entry = entries.value(it.key()).toObject();
        entry.insert("pos", QJsonArray{qRound64(it.value().x()), qRound64(it.value().y())});
        entries.insert(it.key(), entry);
    }
    document.layout.insert("nodes", entries);
    commit(document, mode == QLatin1String("horizontal") || mode == QLatin1String("vertical") ? tr("Distribute")
                                                                                              : tr("Align"));
    return true;
}

// A flag of the selection's layout entries: set on all, or off on all when all have it. One step.
void CanvasModel::toggleLayoutFlag(const QString &flag, const QString &text) {
    const QStringList ids = selectedIds();
    if (ids.isEmpty())
        return;
    Document document = this->scopeDocument();
    QJsonObject entries = document.layout.value("nodes").toObject();
    const bool all = std::all_of(ids.begin(), ids.end(), [&](const QString &id) {
        return entries.value(id).toObject().value(flag).toBool();
    });
    for (const QString &id : ids) {
        QJsonObject entry = entries.value(id).toObject();
        if (all)
            entry.remove(flag);
        else
            entry.insert(flag, true);
        entries.insert(id, entry);
    }
    document.layout.insert("nodes", entries);
    commit(document, text);
}

void CanvasModel::renameNode(const QString &node, const QString &title) {
    if (rowOf(node) < 0)
        return;
    if (isGroup(node)) {
        m_groups->renameGroupCard(node, title);
        return;
    }
    Document document = this->scopeDocument();
    QJsonObject entries = document.layout.value("nodes").toObject();
    QJsonObject entry = entries.value(node).toObject();
    const QString shown = title.trimmed();
    if (shown.isEmpty())
        entry.remove("title");
    else
        entry.insert("title", shown);
    if (entry == entries.value(node).toObject())
        return;
    entries.insert(node, entry);
    document.layout.insert("nodes", entries);
    commit(document, tr("Rename"));
}

// --- clipboard and clones --------------------------------------------------------

Document CanvasModel::copyOf(const QStringList &ids) const {
    const Document all = scopeDocument();
    const QJsonObject entries = all.layout.value("nodes").toObject();
    Document copy;
    QJsonObject copied;
    QStringList types;
    for (const QString &id : ids) {
        copy.graph.nodes.insert(id, all.graph.nodes.value(id));
        copied.insert(id, entries.value(id));
        types << all.graph.nodes.value(id).type;
    }
    copy.layout.insert("nodes", copied);
    copy.present.layout = true;
    // The group definitions the copy uses, and the ones they use, so it pastes anywhere.
    while (!types.isEmpty()) {
        const QString type = types.takeLast();
        const GroupDefinition *group = all.group(type);
        if (!group || copy.groups.contains(type))
            continue;
        copy.groups.insert(type, *group);
        for (const Node &inner : group->nodes)
            types << inner.type;
    }
    copy.present.groups = !copy.groups.isEmpty();
    // The reroute points of the links between the copied nodes, and the inner layout of the
    // copied groups, so the copy looks as the original does.
    const QSet<QString> inCopy(ids.begin(), ids.end());
    const QJsonObject links = all.layout.value("links").toObject();
    QJsonObject copiedLinks;
    for (auto it = links.constBegin(); it != links.constEnd(); ++it) {
        const LinkKey key = parseLinkKey(it.key());
        if (!inCopy.contains(key.node))
            continue;
        for (const auto &[from, item] : linksInto(all, key.node, key.input))
            if (item == key.item && inCopy.contains(from.node))
                copiedLinks.insert(it.key(), it.value());
    }
    if (!copiedLinks.isEmpty())
        copy.layout.insert("links", copiedLinks);
    // A Frame whose nodes are all copied comes with them (5.1: it copies as one).
    QJsonArray copiedFrames;
    for (const QJsonValue &value : all.layout.value("frames").toArray()) {
        const QJsonArray members = value.toObject().value("nodes").toArray();
        if (!members.isEmpty()
            && std::all_of(members.begin(), members.end(), [&](const QJsonValue &id) { return inCopy.contains(id.toString()); }))
            copiedFrames.append(value);
    }
    if (!copiedFrames.isEmpty())
        copy.layout.insert("frames", copiedFrames);
    const QJsonObject groupLayouts = all.layout.value("groups").toObject();
    QJsonObject copiedGroups;
    for (const QString &id : copy.groups.keys())
        if (groupLayouts.contains(id))
            copiedGroups.insert(id, groupLayouts.value(id));
    if (!copiedGroups.isEmpty())
        copy.layout.insert("groups", copiedGroups);
    return copy;
}

QString CanvasModel::copySelection() const {
    const QStringList ids = selectedIds();
    return ids.isEmpty() ? QString() : QString::fromUtf8(copyOf(ids).save());
}

QRectF CanvasModel::boundsOf(const QStringList &ids) const {
    QRectF bounds;
    for (const QString &id : ids)
        if (const int r = rowOf(id); r >= 0)
            bounds = bounds.united(QRectF(m_rows[size_t(r)].pos, QSizeF(m_rows[size_t(r)].shownWidth(), m_rows[size_t(r)].height)));
    return bounds;
}

QStringList CanvasModel::paste(const QString &text, bool connected) {
    const Result<Document> loaded = Document::load(text.toUtf8());
    if (!loaded.ok() || loaded.value().graph.nodes.isEmpty())
        return {};
    std::optional<QPointF> at = m_pointer;
    if (!at && !m_view.isEmpty()) {
        const QVariantList center = m_view.value("center").toList();
        at = QPointF(center.value(0).toDouble(), center.value(1).toDouble());
    }
    const QStringList added = insertCopy(loaded.value(), at.value_or(QPointF(cloneOffset, cloneOffset)), connected,
                                         connected ? tr("Paste with connections") : tr("Paste"), nullptr);
    if (!added.isEmpty())
        emit pasted(loaded.value());
    return added;
}

QStringList CanvasModel::cloneSelection() {
    const QStringList ids = selectedIds();
    if (ids.isEmpty())
        return {};
    return insertCopy(copyOf(ids), boundsOf(ids).topLeft() + QPointF(cloneOffset, cloneOffset), false, tr("Clone"),
                      nullptr);
}

void CanvasModel::beginCloneMove(const QString &node) {
    if (rowOf(node) < 0)
        return;
    if (m_moveOpen || m_cloneOpen)
        endMove();
    const QStringList ids = m_selected.contains(node) ? selectedIds() : QStringList{node};
    beginEdit();
    m_cloneOpen = true;
    const QScopedValueRollback<bool> own(m_gestureEditing, true);
    QHash<QString, QString> copies;
    insertCopy(copyOf(ids), boundsOf(ids).topLeft(), false, tr("Clone"), &copies);
    beginMove(copies.value(node));
}

QStringList CanvasModel::insertCopy(const Document &copy, QPointF topLeft, bool connected, const QString &text,
                                    QHash<QString, QString> *idMap) {
    Document document = this->scopeDocument();
    const QJsonObject entries = copy.layout.value("nodes").toObject();

    // Where each node goes: the copy's own layout, moved so its top left is at topLeft.
    QHash<QString, QPointF> pos;
    int unplaced = 0;
    for (auto it = copy.graph.nodes.cbegin(); it != copy.graph.nodes.cend(); ++it)
        pos.insert(it.key(), point(entries.value(it.key()).toObject().value("pos"))
                                 .value_or(QPointF(unplaced++ * (m_defaultWidth + pasteGap), 0)));
    QPointF corner(std::numeric_limits<qreal>::max(), std::numeric_limits<qreal>::max());
    for (const QPointF &p : pos)
        corner = QPointF(std::min(corner.x(), p.x()), std::min(corner.y(), p.y()));

    // A group the copy brings under an id this workflow already uses for another definition
    // comes in under a new id, so the pasted instance keeps its own ports (a workflow holds
    // one definition per id, 4.4).
    QHash<QString, QString> renamed;
    for (auto it = copy.groups.cbegin(); it != copy.groups.cend(); ++it) {
        if (!document.groups.contains(it.key()) || groupJson(document.groups.value(it.key())) == groupJson(it.value()))
            continue;
        QString id;
        for (int n = 2; id.isEmpty() || document.groups.contains(id) || copy.groups.contains(id); ++n)
            id = QStringLiteral("%1-%2").arg(it.key()).arg(n);
        renamed.insert(it.key(), id);
    }
    auto typeOf = [&renamed](const QString &type) { return renamed.value(type, type); };

    QHash<QString, QString> ids; // copied id -> new id
    QSet<QString> fresh;
    for (auto it = copy.graph.nodes.cbegin(); it != copy.graph.nodes.cend(); ++it) {
        const QString id = newNodeId(document.graph.nodes);
        document.graph.nodes.insert(id, Node{});
        ids.insert(it.key(), id);
        fresh.insert(id);
    }
    QJsonObject layoutNodes = document.layout.value("nodes").toObject();
    QList<std::pair<QString, QString>> external; // (new node, input) holding a link from outside
    for (auto it = copy.graph.nodes.cbegin(); it != copy.graph.nodes.cend(); ++it) {
        Node node = it.value();
        node.type = typeOf(node.type);
        const QString id = ids.value(it.key());
        for (auto in = node.inputs.begin(); in != node.inputs.end(); ++in) {
            // A link between copied nodes follows them; one from outside stays only with
            // `connected`, and only from a node this workflow has.
            const auto links = linksIn(in.value());
            bool outside = false;
            QList<InputValue> items =
                in.value().kind() == InputValue::Kind::List ? in.value().items() : QList<InputValue>{in.value()};
            for (const auto &[from, item] : links) {
                InputValue &slot = items[item < 0 ? 0 : item];
                if (ids.contains(from.node))
                    slot = InputValue::link({ids.value(from.node), from.port});
                else if (connected && document.graph.nodes.contains(from.node) && !fresh.contains(from.node))
                    outside = true;
                else
                    slot = InputValue();
            }
            if (links.isEmpty())
                continue;
            items.erase(std::remove_if(items.begin(), items.end(), [](const InputValue &v) { return v.isEmpty(); }),
                        items.end());
            if (in.value().kind() == InputValue::Kind::List)
                in.value() = items.isEmpty() ? InputValue() : InputValue::list(items);
            else
                in.value() = items.value(0);
            if (outside)
                external.append({id, in.key()});
        }
        node.inputs.removeIf([](const auto &entry) { return entry.value().isEmpty(); });
        document.graph.nodes.insert(id, node);

        QJsonObject entry = entries.value(it.key()).toObject();
        const QPointF at = pos.value(it.key()) - corner + topLeft;
        entry.insert("pos", QJsonArray{qRound64(at.x()), qRound64(at.y())});
        layoutNodes.insert(id, entry);
    }
    document.layout.insert("nodes", layoutNodes);
    QJsonObject groupLayouts = document.layout.value("groups").toObject();
    const QJsonObject copiedLayouts = copy.layout.value("groups").toObject();
    for (auto it = copy.groups.cbegin(); it != copy.groups.cend(); ++it) {
        const QString id = typeOf(it.key());
        if (document.groups.contains(id))
            continue;
        GroupDefinition group = it.value();
        for (Node &inner : group.nodes)
            inner.type = typeOf(inner.type);
        document.groups.insert(id, group);
        document.present.groups = true;
        if (copiedLayouts.contains(it.key()) && !groupLayouts.contains(id))
            groupLayouts.insert(id, copiedLayouts.value(it.key()));
    }
    if (!groupLayouts.isEmpty())
        document.layout.insert("groups", groupLayouts);
    // The copy's Frames, on its new ids, moved with its nodes.
    const QJsonArray copiedFrames = copy.layout.value("frames").toArray();
    if (!copiedFrames.isEmpty()) {
        QJsonArray frames = document.layout.value("frames").toArray();
        QSet<QString> taken;
        for (const QJsonValue &frame : frames)
            taken.insert(frame.toObject().value("id").toString());
        const QPointF shift = topLeft - corner;
        for (const QJsonValue &value : copiedFrames) {
            QJsonObject frame = value.toObject();
            const QJsonArray rect = frame.value("rect").toArray();
            if (rect.size() != 4)
                continue;
            frame.insert("rect", QJsonArray{qRound64(rect[0].toDouble() + shift.x()), qRound64(rect[1].toDouble() + shift.y()),
                                            rect[2], rect[3]});
            QJsonArray members;
            for (const QJsonValue &id : frame.value("nodes").toArray())
                if (ids.contains(id.toString()))
                    members.append(ids.value(id.toString()));
            frame.insert("nodes", members);
            QString id;
            for (int n = 1; id.isEmpty() || taken.contains(id); ++n)
                id = QStringLiteral("f%1").arg(n);
            taken.insert(id);
            frame.insert("id", id);
            frames.append(frame);
        }
        document.layout.insert("frames", frames);
    }
    // The copy's reroute points, on its new ids, moved with its nodes.
    const QJsonObject copiedLinks = copy.layout.value("links").toObject();
    if (!copiedLinks.isEmpty()) {
        QJsonObject links = document.layout.value("links").toObject();
        const QPointF shift = topLeft - corner;
        for (auto it = copiedLinks.constBegin(); it != copiedLinks.constEnd(); ++it) {
            LinkKey key = parseLinkKey(it.key());
            if (!ids.contains(key.node))
                continue;
            key.node = ids.value(key.node);
            QJsonObject entry = it.value().toObject();
            QJsonArray points;
            for (const QJsonValue &p : entry.value("points").toArray())
                if (const auto at = point(p))
                    points.append(QJsonArray{qRound64(at->x() + shift.x()), qRound64(at->y() + shift.y())});
            entry.insert("points", points);
            links.insert(key.text(), entry);
        }
        document.layout.insert("links", links);
    }

    // Inside a group, a copy holding an instance of that group (or of one around it) would
    // make it contain itself (5.2): nothing is pasted.
    for (const QString &id : std::as_const(fresh)) {
        const QString type = document.graph.nodes.value(id).type;
        if (document.group(type) && !canHoldGroup(document, type)) {
            m_groups->setProblem(tr("A group cannot hold itself: the copy has an instance of %1, which holds the group shown.")
                                .arg(document.group(type)->name.isEmpty() ? type : document.group(type)->name));
            return {};
        }
    }

    beginEdit();
    commit(document, text);
    if (!external.isEmpty()) {
        // A link from outside that cannot feed its input (another type, a cycle) is dropped.
        Document checked = this->scopeDocument();
        bool dropped = false;
        for (const auto &[id, input] : external) {
            Node &node = checked.graph.nodes[id];
            const InputValue kept = withoutLinks(node.inputs.value(input), [&](const PortRef &from) {
                return !fresh.contains(from.node) && !problem(from.node, from.port, id, input, false).isEmpty();
            });
            if (kept != node.inputs.value(input)) {
                dropped = true;
                if (kept.isEmpty())
                    node.inputs.remove(input);
                else
                    node.inputs.insert(input, kept);
            }
        }
        if (dropped)
            commit(checked, text);
    }
    QStringList added;
    for (auto it = copy.graph.nodes.cbegin(); it != copy.graph.nodes.cend(); ++it)
        added << ids.value(it.key());
    select(added, QStringLiteral("replace"));
    endEdit(text);
    if (idMap)
        *idMap = ids;
    return added;
}

void CanvasModel::setPointer(const QVariant &point) {
    if (point.canConvert<QPointF>() && point.isValid())
        m_pointer = point.toPointF();
    else
        m_pointer.reset();
}

// --- view ----------------------------------------------------------------------

bool CanvasModel::fitView(qreal width, qreal height, qreal reservedTop, qreal reservedBottom) {
    QStringList ids = selectedIds();
    if (ids.isEmpty())
        for (const NodeRow &row : m_rows)
            ids << row.id;
    const QRectF bounds = boundsOf(ids);
    // What floats over the canvas's top or bottom edge (the run toolbar) is not room for nodes.
    const qreal room = height - reservedTop - reservedBottom;
    if (bounds.isEmpty() || width <= 0 || room <= 0)
        return false;
    // The bounds fill three quarters of the room, a margin all round.
    const qreal zoom = std::clamp(std::min(width / bounds.width(), room / bounds.height()) * 0.75, minZoom, maxZoom);
    // The room's middle is not the canvas's: the world point at the canvas's middle sits that far
    // (in pixels) from the bounds' center.
    const qreal shift = (reservedTop - reservedBottom) / 2 / zoom;
    m_view = {{"center", QVariantList{bounds.center().x(), bounds.center().y() - shift}}, {"zoom", zoom}};
    return true;
}

// --- sections (5.4, 6.1) ---------------------------------------------------------------

QString CanvasModel::advancedHeaderAt(qreal x, qreal y) const {
    for (int r = int(m_rows.size()) - 1; r >= 0; --r) {
        const NodeRow &row = m_rows[size_t(r)];
        if (row.collapsed || x < row.pos.x() || x > row.pos.x() + row.shownWidth())
            continue;
        for (const Port &port : row.inputs)
            if (port.header == QLatin1String(kAdvancedSection) && y >= row.pos.y() + port.headerTop
                && y < row.pos.y() + port.headerTop + m_rowHeight)
                return row.id;
    }
    return {};
}

void CanvasModel::toggleAdvanced(const QString &node) {
    const int r = rowOf(node);
    if (r < 0)
        return;
    Document document = scopeDocument();
    QJsonObject entries = document.layout.value("nodes").toObject();
    QJsonObject entry = entries.value(node).toObject();
    const bool open = !m_rows[size_t(r)].advancedOpen;
    if (open)
        entry.insert("advanced", true);
    else
        entry.remove("advanced");
    entries.insert(node, entry);
    document.layout.insert("nodes", entries);
    commit(document, open ? tr("Show advanced inputs") : tr("Fold advanced inputs"));
}
