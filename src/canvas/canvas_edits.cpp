// The canvas model's edits and their undo stack (spec 12.4, 12.11): how a changed document
// becomes the rows on screen, the edit bounds and the undo command, and the edits of the
// selection (delete, bypass, mute, collapse, copy, paste, clone). canvas_model.h explains
// the design.

#include "canvas/canvas_model.h"
#include "canvas/input_links.h"

#include "workflow/content_hash.h"

#include <QJsonArray>
#include <QJsonObject>
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

} // namespace

// One undo step: the document before and after. Pushing it does nothing more, since the edit
// is already made.
class EditCommand : public QUndoCommand {
public:
    EditCommand(CanvasModel *canvas, CanvasModel::Snapshot before, CanvasModel::Snapshot after, const QString &text)
        : QUndoCommand(text), m_canvas(canvas), m_before(std::move(before)), m_after(std::move(after)) {}

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

void CanvasModel::restore(const Snapshot &target, bool selectAdded) {
    const Document &doc = target.document;
    const Nodes oldNodes = m_document.graph.nodes;
    const QJsonObject oldLayout = m_document.layout;
    const QJsonObject oldEntries = oldLayout.value("nodes").toObject();
    const QJsonObject newEntries = doc.layout.value("nodes").toObject();
    QSet<QString> affected, moved;
    QStringList added;
    bool selectionLost = false;

    // Nodes that are gone.
    for (int r = int(m_rows.size()) - 1; r >= 0; --r) {
        const QString id = m_rows[size_t(r)].id;
        if (doc.graph.nodes.contains(id))
            continue;
        affected.insert(id);
        if (m_selected.remove(id))
            selectionLost = true;
        m_nodeList->beginRemoveRows({}, r, r);
        m_rows.erase(m_rows.begin() + r);
        m_rowOf.clear();
        for (int i = 0; i < int(m_rows.size()); ++i)
            m_rowOf.insert(m_rows[size_t(i)].id, i);
        m_nodeList->endRemoveRows();
    }

    // The viewport is not part of an edit: keep the one on screen.
    m_document = doc;

    // Nodes that changed are made again; nodes that only moved, moved.
    for (int r = 0; r < int(m_rows.size()); ++r) {
        NodeRow &row = m_rows[size_t(r)];
        const QPointF pos = point(newEntries.value(row.id).toObject().value("pos")).value_or(row.pos);
        if (!sameNode(oldNodes.value(row.id), doc.graph.nodes.value(row.id))
            || withoutPos(oldEntries.value(row.id).toObject()) != withoutPos(newEntries.value(row.id).toObject())) {
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

// Remakes the link rows into and out of these nodes; ports at either end show their new state.
void CanvasModel::relink(const QSet<QString> &nodes) {
    if (nodes.isEmpty())
        return;
    QSet<QString> ends;
    for (int i = int(m_links.size()) - 1; i >= 0; --i) {
        const LinkRow &link = m_links[size_t(i)];
        if (!nodes.contains(link.toNode) && !nodes.contains(link.from.node))
            continue;
        ends << link.toNode << link.from.node;
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
                row.color = out ? colorKey(out->type, out->kind) : QStringLiteral("unknown");
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
        }
        m_linkList->endInsertRows();
    }
    for (const QString &id : ends)
        refreshPorts(id);
    emit reroutesChanged();
}

// --- edit bounds and the undo stack ---------------------------------------------

void CanvasModel::beginEdit() {
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
    restore({document, order}, false);
    endEdit(text);
}

void CanvasModel::finishGestures() {
    if (!m_drag.isEmpty())
        endLinkDrag();
    if (m_moveOpen || m_cloneOpen)
        endMove();
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
    Document document = this->document();
    for (const QString &id : ids)
        document.graph.nodes.remove(id);
    for (Node &node : document.graph.nodes)
        stripLinks(node, [&](const PortRef &from) { return gone.contains(from.node); });

    // Their layout goes too: entries, reroute points into them, places in Frames.
    QJsonObject layout = document.layout;
    QJsonObject entries = layout.value("nodes").toObject();
    for (const QString &id : ids)
        entries.remove(id);
    layout.insert("nodes", entries);
    if (layout.contains("links")) {
        QJsonObject links = layout.value("links").toObject();
        for (const QString &key : links.keys())
            if (gone.contains(key.section('.', 0, 0)))
                links.remove(key);
        layout.insert("links", links);
    }
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
    Document document = this->document();
    const bool all = std::all_of(ids.begin(), ids.end(),
                                 [&](const QString &id) { return document.graph.nodes.value(id).mode == *mode; });
    for (const QString &id : ids)
        document.graph.nodes[id].mode = all ? NodeMode::Normal : *mode;
    commit(document, *mode == NodeMode::Bypass ? tr("Bypass") : tr("Mute"));
}

void CanvasModel::toggleCollapsed() {
    const QStringList ids = selectedIds();
    if (ids.isEmpty())
        return;
    Document document = this->document();
    QJsonObject entries = document.layout.value("nodes").toObject();
    const bool all = std::all_of(ids.begin(), ids.end(), [&](const QString &id) {
        return entries.value(id).toObject().value("collapsed").toBool();
    });
    for (const QString &id : ids) {
        QJsonObject entry = entries.value(id).toObject();
        if (all)
            entry.remove("collapsed");
        else
            entry.insert("collapsed", true);
        entries.insert(id, entry);
    }
    document.layout.insert("nodes", entries);
    commit(document, tr("Collapse"));
}

// --- clipboard and clones --------------------------------------------------------

Document CanvasModel::copyOf(const QStringList &ids) const {
    const Document all = document();
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
            bounds = bounds.united(QRectF(m_rows[size_t(r)].pos, QSizeF(m_rows[size_t(r)].width, m_rows[size_t(r)].height)));
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
    return insertCopy(loaded.value(), at.value_or(QPointF(cloneOffset, cloneOffset)), connected,
                      connected ? tr("Paste with connections") : tr("Paste"), nullptr);
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
    QHash<QString, QString> copies;
    insertCopy(copyOf(ids), boundsOf(ids).topLeft(), false, tr("Clone"), &copies);
    beginMove(copies.value(node));
}

QStringList CanvasModel::insertCopy(const Document &copy, QPointF topLeft, bool connected, const QString &text,
                                    QHash<QString, QString> *idMap) {
    Document document = this->document();
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
    for (auto it = copy.groups.cbegin(); it != copy.groups.cend(); ++it) {
        if (!document.groups.contains(it.key())) {
            document.groups.insert(it.key(), it.value());
            document.present.groups = true;
        }
    }

    beginEdit();
    commit(document, text);
    if (!external.isEmpty()) {
        // A link from outside that cannot feed its input (another type, a cycle) is dropped.
        Document checked = this->document();
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

bool CanvasModel::fitView(qreal width, qreal height) {
    QStringList ids = selectedIds();
    if (ids.isEmpty())
        for (const NodeRow &row : m_rows)
            ids << row.id;
    const QRectF bounds = boundsOf(ids);
    if (bounds.isEmpty() || width <= 0 || height <= 0)
        return false;
    // The bounds fill three quarters of the canvas, a margin all round.
    const qreal zoom = std::clamp(std::min(width / bounds.width(), height / bounds.height()) * 0.75, minZoom, maxZoom);
    m_view = {{"center", QVariantList{bounds.center().x(), bounds.center().y()}}, {"zoom", zoom}};
    return true;
}
