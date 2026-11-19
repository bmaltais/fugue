#pragma once

// The canvas's model of one workflow (spec 12): the document's nodes and links as two
// list models the QML canvas draws (src/canvas/*.qml), and the edits the canvas makes.
//
// The document is the truth. The model holds a fugue::Document, reads node types from
// the Registry it is given (built-in manifests now, packs from M3) and works out each
// node's face from them: which ports it has, their type, shape and color, and where on
// the node they sit. Port positions are computed here, not in QML, so a wire's ends are
// plain numbers (x1, y1, x2, y2 on the link rows) and moving a node updates only its own
// row and the rows of its links. Link checks use the engine's own rules (port_type.h
// compatibility and explain), plus the ones the compiler would reject: a node into
// itself, a cycle, a node type that is not installed. connect() checks again, so QML is
// never trusted.
//
// Nodes whose type is not installed show as placeholders with the inputs and links the
// document has (3.6), so nothing is lost. A group instance (4.4) shows its definition's
// exposed inputs and outputs. Nodes the layout does not place (an agent-written file)
// are put in columns by how far downstream they are.
//
// Edits and undo (#17, spec 12.11): every edit is one command on the model's undo stack, one
// stack per workflow. An edit takes the document as document() gives it (positions folded
// in), changes it, and hands it to restore(), which works out what differs by node id and
// updates only those rows and their links; the command keeps the document before and after,
// so undo and redo are the same restore. A gesture is one command however many steps it
// takes: a drag (beginMove to endMove), a link dragged from a port (beginLinkDrag to
// endLinkDrag, a picked-up link included), an Alt+drag clone and its move. An edit that
// changes nothing pushes nothing. The viewport and the selection are not edits.
//
// setDocument() starts the document with an empty undo stack.
//
// The seam for the tickets that build on this:
// - #20 save, open, recovery (done): Workflows swaps each tab's document in with
//   setDocument() and reads it back with document(); document() writes positions back into
//   layout.nodes.<id>.pos and the viewport into layout.view, keeping every other layout key.
//   Each tab keeps its own undo stack: takeUndoStack() before another tab's document comes
//   in, setUndoStack() when the tab is shown again. `edited` fires after every change to
//   the content, undo and redo included; "unsaved" is workflow/content_hash.h, by content.
// - #18 search (done): NodeSearch, owned here as `search`, adds a node with addNode(type,
//   x, y) and links it with connectPorts inside beginEdit()/endEdit(), so the two are one
//   undo step; the canvas emits searchRequested (double-click on empty canvas) and
//   linkReleased (a dragged link let go on empty canvas) with the world point and the
//   dragged port, for search filtered by port type.
// - #19 run state: add roles to NodeList (state, progress, last output values); a node's
//   row is found by id in O(1) (rowOf).
// - The window's commands (edit.*, node.bypass, node.mute, node.collapse, view.*) call the
//   edits below; canvas_commands.h registers them.
// - Text size: FugueCanvas binds textSize to the theme's; rows and titles follow it.

#include "engine/document.h"
#include "engine/port_type.h"
#include "search/node_search.h"

#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>
#include <vector>

namespace fugue {
class Registry;
}

class CanvasModel;
class QUndoStack;

// One row per node, in drawing order (later rows on top).
class NodeList : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        NodeId = Qt::UserRole + 1,
        TypeId,
        Title,
        Missing,   // the type is not installed: a placeholder
        X,
        Y,
        Width,
        Height,
        Collapsed,
        Mode,      // "normal", "bypass" or "mute" (3.5)
        Selected,
        Inputs,    // list of port maps, see CanvasModel::portMap
        Outputs,
    };

    explicit NodeList(CanvasModel *canvas);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    friend class CanvasModel;
class QUndoStack;
    CanvasModel *m_canvas;
};

// One row per link, a list input's items each their own.
class LinkList : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        FromNode = Qt::UserRole + 1,
        FromPort,
        ToNode,
        ToPort,
        Item,  // the list item it fills, or -1 for the whole value
        Color, // the source's color key, see CanvasModel::colorKey
        X1,    // the output's end, in world coordinates
        Y1,
        X2,    // the input's end
        Y2,
        Path,  // the wire as SVG path data, through the layout's reroute points
    };

    explicit LinkList(CanvasModel *canvas);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    friend class CanvasModel;
class QUndoStack;
    CanvasModel *m_canvas;
};

class CanvasModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(NodeList *nodes READ nodes CONSTANT)
    Q_PROPERTY(LinkList *links READ links CONSTANT)
    Q_PROPERTY(QVariantList frames READ frames NOTIFY framesChanged)
    Q_PROPERTY(QVariantMap view READ view NOTIFY documentReset)
    // Node search (src/search/node_search.h): it adds nodes through addNode() and connectPorts().
    Q_PROPERTY(fugue::NodeSearch *search READ search CONSTANT)
    // The base text size in px (theme.textSize); a node's title and rows are sized from it.
    Q_PROPERTY(int textSize READ textSize WRITE setTextSize NOTIFY metricsChanged)
    Q_PROPERTY(qreal headerHeight READ headerHeight NOTIFY metricsChanged)
    Q_PROPERTY(qreal rowHeight READ rowHeight NOTIFY metricsChanged)
    // While a link is dragged: the port it started from ({node, port, output, color, x, y}),
    // else empty. Ports bind their dimming to this, so it changes at the start and end only.
    Q_PROPERTY(QVariantMap linkDrag READ linkDrag NOTIFY linkDragChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY undoChanged)
    Q_PROPERTY(QVariant pointer READ pointer WRITE setPointer)

public:
    explicit CanvasModel(const fugue::Registry *registry, QObject *parent = nullptr);
    ~CanvasModel() override;

    NodeList *nodes() const { return m_nodeList; }
    LinkList *links() const { return m_linkList; }
    fugue::NodeSearch *search() const { return m_search; }

    void setDocument(const fugue::Document &document);
    // The document with the canvas's positions and viewport in its layout.
    fugue::Document document() const;

    QVariantList frames() const;
    // {center: [x, y], zoom} from layout.view, or empty when the file has none.
    QVariantMap view() const;
    Q_INVOKABLE void setView(qreal centerX, qreal centerY, qreal zoom);

    int textSize() const { return m_textSize; }
    // Lays every node and wire out again: one reset of each list.
    void setTextSize(int size);
    qreal headerHeight() const { return m_headerHeight; }
    qreal rowHeight() const { return m_rowHeight; }
    static constexpr qreal defaultWidth = 220;

    // --- hit tests, in world coordinates ---

    // The port within `radius` of the point, nearest first: {node, port, output}, or empty.
    Q_INVOKABLE QVariantMap portAt(qreal x, qreal y, qreal radius) const;
    // The topmost node under the point, or empty.
    Q_INVOKABLE QString nodeAt(qreal x, qreal y) const;
    // Every node the rectangle touches, as ComfyUI's marquee selects.
    Q_INVOKABLE QStringList nodesIn(const QRectF &rect) const;

    // --- links ---

    // Why the output `fromNode.output` cannot feed the input `toNode.input`, in one
    // translated sentence; empty when it can.
    Q_INVOKABLE QString linkProblem(const QString &fromNode, const QString &output, const QString &toNode,
                                    const QString &input) const;
    // Links the output into the input when linkProblem() is empty. A single input's old link
    // is replaced; a list input gains an item at `item` (-1: at the end). False when refused.
    Q_INVOKABLE bool connectPorts(const QString &fromNode, const QString &output, const QString &toNode,
                                  const QString &input, int item = -1);
    // Removes the link into the input (one list item, or all of them with -1). False when
    // there was none.
    Q_INVOKABLE bool disconnectInput(const QString &node, const QString &input, int item = -1);

    // Dragging a link (12.6): start from a port. From an output, or from an unlinked input,
    // the wire runs from that port. From a linked single input the link is picked up, as in
    // ComfyUI: it is removed and the drag continues from its source output.
    Q_INVOKABLE void beginLinkDrag(const QString &node, const QString &port, bool output);
    QVariantMap linkDrag() const { return m_drag; }
    // Why the dragged link cannot end on this port; empty when it can. Cheap: what the drag
    // cannot reach is worked out once when it starts.
    Q_INVOKABLE QString dragProblem(const QString &node, const QString &port, bool output) const;
    // Ends the drag on a port (connecting when allowed), or anywhere else with an empty node.
    Q_INVOKABLE bool endLinkDrag(const QString &node = {}, const QString &port = {}, bool output = false);

    // Removes every link into the input (all list items) or out of the output (12.4:
    // Ctrl+Alt+click on a port). False when there was none.
    Q_INVOKABLE bool disconnectPort(const QString &node, const QString &port, bool output);
    // Shift+drag from an output moves all its links (12.4): the drag holds the inputs they
    // lead to and looks for another output; endLinkDrag on one that can feed every one of
    // them moves them all, anywhere else leaves them as they were. With no links it is a
    // plain drag from the output.
    Q_INVOKABLE void beginLinksMove(const QString &node, const QString &output);
    // The type of a port of a node on the canvas, or nothing when the node or port is not
    // there or its type is not known (a placeholder's port). Node search uses it to filter
    // by the port a link was dragged from.
    std::optional<fugue::PortSpec> portType(const QString &node, const QString &port, bool output) const;

    // --- nodes ---

    // A new node of an installed type at the point; its id, or empty for an unknown type.
    Q_INVOKABLE QString addNode(const QString &type, qreal x, qreal y);
    // Removes the selected nodes and every link into or out of them.
    Q_INVOKABLE void deleteSelection();
    // Bypass or mute (3.5) the selection, `mode` being "bypass" or "mute": every selected
    // node takes the mode, unless all of them have it already, and then all go back to normal.
    Q_INVOKABLE void toggleMode(const QString &mode);
    // Collapses the selection, or expands it when all of it is collapsed already.
    Q_INVOKABLE void toggleCollapsed();

    // --- clipboard ---

    // The selection as a workflow document (a `.fugue` file of just those nodes, their
    // layout, and the group definitions they use); empty when nothing is selected. Links to
    // nodes outside it are kept, so a paste with connections can restore them.
    Q_INVOKABLE QString copySelection() const;
    // Adds the nodes of a copied document (any `.fugue` text) with new ids, the copy's top
    // left at the pointer (else the view's center), and selects them. Links between them
    // are kept; with `connected` (Ctrl+Shift+V), so are links from nodes still in this
    // workflow that can feed them. Their ids, or none when the text is not a workflow.
    Q_INVOKABLE QStringList paste(const QString &text, bool connected = false);
    // Ctrl+D: a copy of the selection beside it, selected. Links between them are kept.
    Q_INVOKABLE QStringList cloneSelection();
    // Alt+drag: copies the node (the selection when the node is in it) where it is, selects
    // the copies and starts moving them; endMove() ends the gesture, one undo step in all.
    Q_INVOKABLE void beginCloneMove(const QString &node);
    // The pointer over the canvas, in world coordinates, where paste puts nodes; undefined
    // (clearPointer) when it is not over the canvas. The canvas binds it.
    QVariant pointer() const { return m_pointer ? QVariant(*m_pointer) : QVariant(); }
    void setPointer(const QVariant &point);
    void setPointer(qreal x, qreal y) { m_pointer = QPointF(x, y); }
    void clearPointer() { m_pointer.reset(); }

    // --- undo (12.11) ---

    QUndoStack *undoStack() const { return m_undo.get(); }
    // Hands the undo stack over (a tab keeps it while another is shown) and starts an empty one.
    std::unique_ptr<QUndoStack> takeUndoStack();
    // Uses this stack, taken earlier for the document now set, in place of the current one.
    void setUndoStack(std::unique_ptr<QUndoStack> stack);
    // Brackets several edits into one undo step: the first begin takes the document as it
    // is, the last end pushes one command named `text` when the content changed.
    void beginEdit();
    void endEdit(const QString &text);
    bool canUndo() const;
    bool canRedo() const;
    QString undoText() const;
    QString redoText() const;
    // A gesture still in progress is finished first.
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // --- view commands ---

    // Sets the view to fit the selection, or every node when none is selected, in a canvas
    // of this size (ComfyUI's `.`). False when there are no nodes.
    Q_INVOKABLE bool fitView(qreal width, qreal height);
    // Ask the canvas to zoom by the factor at its center, or to fit (fitRequested).
    void requestZoom(qreal factor) { emit zoomRequested(factor); }
    void requestFit() { emit fitRequested(); }
    static constexpr qreal minZoom = 0.1;
    static constexpr qreal maxZoom = 4;

    // Moving: beginMove picks what moves (the selection when the node is in it, else just
    // the node), moveBy offsets them from where they started, endMove finishes.
    Q_INVOKABLE void beginMove(const QString &node);
    Q_INVOKABLE void moveBy(qreal dx, qreal dy);
    Q_INVOKABLE void endMove();
    Q_INVOKABLE QPointF position(const QString &node) const;

    // --- selection ---

    // mode: "replace", "add", "subtract" or "toggle".
    Q_INVOKABLE void select(const QStringList &ids, const QString &mode = QStringLiteral("replace"));
    Q_INVOKABLE QStringList selectedIds() const;
    Q_INVOKABLE void selectAll();

    // A port type's color and shape (12.6). The color is a key for the theme's portColor()
    // (a built-in type's id, or "declared"), or "any" or "unknown" (a placeholder's port);
    // the QML picks the color. The shape: circle, square, diamond or ring.
    static QString colorKey(const std::optional<fugue::PortSpec> &type, fugue::PortKind kind);
    static QString shape(const std::optional<fugue::PortSpec> &type, fugue::PortKind kind);

signals:
    void documentReset();
    void framesChanged();
    void metricsChanged();
    void linkDragChanged();
    // An edit changed the document (not a selection or a viewport change), undo and redo
    // included.
    void edited();
    void undoChanged();
    void zoomRequested(qreal factor);
    void fitRequested();

private:
    friend class NodeList;
    friend class LinkList;
    friend class EditCommand;

    // The document with positions folded in, and the nodes in drawing order.
    struct Snapshot {
        fugue::Document document;
        QStringList order;
    };

    struct Port {
        QString name;
        QString label;
        bool output = false;
        std::optional<fugue::PortSpec> type; // none: unknown (a placeholder's port)
        fugue::PortKind kind = fugue::PortKind::Unknown;
        bool widget = false; // a value input, shown in the widget band
        bool required = false;
        QJsonValue defaultValue = QJsonValue::Undefined;
        qreal y = 0; // from the node's top
    };
    struct NodeRow {
        QString id;
        QString title;
        bool missing = false;
        QPointF pos;
        qreal width = defaultWidth;
        qreal height = 0;
        bool collapsed = false;
        std::vector<Port> inputs;
        std::vector<Port> outputs;
    };
    struct LinkRow {
        fugue::PortRef from;
        QString toNode;
        QString toInput;
        int item = -1;
        QString color;
        QPointF a, b;
        QString path;
    };

    Snapshot snapshot() const;
    // Makes the document this one: only nodes that differ are touched, and their links.
    // With selectAdded, nodes that come back are selected (undo of a delete, redo of a paste).
    void restore(const Snapshot &target, bool selectAdded);
    void relink(const QSet<QString> &nodes);
    // One edit: the document is document() changed by the caller.
    void commit(const fugue::Document &document, const QString &text);
    void finishGestures();
    void contentChanged();
    void watchUndoStack();
    // Adds a copy's nodes with new ids, its top left at topLeft, as one edit; selects them.
    QStringList insertCopy(const fugue::Document &copy, QPointF topLeft, bool connected, const QString &text,
                           QHash<QString, QString> *idMap);
    fugue::Document copyOf(const QStringList &ids) const;
    QRectF boundsOf(const QStringList &ids) const;

    void rebuildLinks();
    void layOut(NodeRow &row) const;
    void autoPlace(const QSet<QString> &unplaced);
    NodeRow makeRow(const QString &id) const;
    void portsFromType(NodeRow &row, const fugue::Node &node) const;
    void refreshPorts(const QString &nodeId); // after a link change: wired flags
    void refreshLinkEnds(int linkRow);
    void emitNodeChanged(int row, const QList<int> &roles);
    QVariantList portMaps(const NodeRow &row, bool outputs) const;
    QVariantMap portMap(const NodeRow &row, const Port &port) const;
    const Port *findPort(const QString &node, const QString &port, bool output) const;
    QString problem(const QString &fromNode, const QString &output, const QString &toNode, const QString &input,
                    bool useDrag) const;
    QSet<QString> upstreamOf(const QString &node) const;
    QSet<QString> downstreamOf(const QString &node) const;
    bool isWired(const QString &node, const QString &port, bool output) const;
    QString displayValue(const fugue::InputValue &value, const Port &port, bool *isDefault) const;
    int rowOf(const QString &node) const { return m_rowOf.value(node, -1); }

    const fugue::Registry *m_registry;
    fugue::Document m_document;
    NodeList *m_nodeList;
    LinkList *m_linkList;
    fugue::NodeSearch *m_search;
    std::vector<NodeRow> m_rows;
    QHash<QString, int> m_rowOf;
    std::vector<LinkRow> m_links;
    QHash<QString, QList<int>> m_linksOf; // node id -> link rows touching it
    QSet<QString> m_selected;
    QVariantMap m_view;
    int m_textSize = 12;
    qreal m_headerHeight = 26; // textSize * 13 / 6, rounded
    qreal m_rowHeight = 22;    // textSize * 11 / 6, rounded

    QVariantMap m_drag;
    QSet<QString> m_dragUnreachable; // nodes the dragged link would make a cycle with

    QStringList m_moving;
    std::vector<QPointF> m_moveStart;
    bool m_moveOpen = false;  // a move holds an edit open
    bool m_cloneOpen = false; // an Alt+drag clone holds one more
    bool m_dragOpen = false;  // a link drag holds an edit open

    std::unique_ptr<QUndoStack> m_undo;
    int m_editDepth = 0;
    Snapshot m_editBefore;
    std::optional<QPointF> m_pointer;
};
