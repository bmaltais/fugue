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
// The seam for the tickets that build on this:
// - #17 undo, selection, clipboard: every edit goes through the Q_INVOKABLE edits below
//   (connect, disconnect, the moves, addNode). Each is small and has an inverse made of
//   the others (a connect that replaced a link is undone by connecting the old source
//   with its item index; a move by beginMove/moveBy/endMove back), so an undo command can
//   wrap them. endMove() marks one finished drag. Selection is the `selected` role and
//   select()/selectedIds(); the marquee and Shift/Ctrl+click already use it.
// - #18 search: addNode(type, x, y); the canvas emits searchRequested (double-click on
//   empty canvas) and linkReleased (a dragged link let go on empty canvas) with the
//   world point and the dragged port, for search filtered by port type.
// - #19 run state: add roles to NodeList (state, progress, last output values); a node's
//   row is found by id in O(1) (rowOf).
// - #20 save, open: setDocument() and document(); document() writes positions back into
//   layout.nodes.<id>.pos and the viewport into layout.view, keeping every other layout key.
// - Text size: FugueCanvas binds rowHeight and headerHeight to the theme's text size;
//   either setter lays every node out again.

#include "engine/document.h"
#include "engine/port_type.h"

#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include <optional>
#include <vector>

namespace fugue {
class Registry;
}

class CanvasModel;

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
    CanvasModel *m_canvas;
};

class CanvasModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(NodeList *nodes READ nodes CONSTANT)
    Q_PROPERTY(LinkList *links READ links CONSTANT)
    Q_PROPERTY(QVariantList frames READ frames NOTIFY framesChanged)
    Q_PROPERTY(QVariantMap view READ view NOTIFY documentReset)
    Q_PROPERTY(qreal headerHeight READ headerHeight WRITE setHeaderHeight NOTIFY metricsChanged)
    Q_PROPERTY(qreal rowHeight READ rowHeight WRITE setRowHeight NOTIFY metricsChanged)
    // While a link is dragged: the port it started from ({node, port, output, color, x, y}),
    // else empty. Ports bind their dimming to this, so it changes at the start and end only.
    Q_PROPERTY(QVariantMap linkDrag READ linkDrag NOTIFY linkDragChanged)

public:
    explicit CanvasModel(const fugue::Registry *registry, QObject *parent = nullptr);
    ~CanvasModel() override;

    NodeList *nodes() const { return m_nodeList; }
    LinkList *links() const { return m_linkList; }

    void setDocument(const fugue::Document &document);
    // The document with the canvas's positions and viewport in its layout.
    fugue::Document document() const;

    QVariantList frames() const;
    // {center: [x, y], zoom} from layout.view, or empty when the file has none.
    QVariantMap view() const;
    Q_INVOKABLE void setView(qreal centerX, qreal centerY, qreal zoom);

    qreal headerHeight() const { return m_headerHeight; }
    qreal rowHeight() const { return m_rowHeight; }
    void setHeaderHeight(qreal height);
    void setRowHeight(qreal height);
    static constexpr qreal defaultWidth = 220;

    // --- hit tests, in world coordinates ---

    // The port within `radius` of the point, nearest first: {node, port, output}, or empty.
    Q_INVOKABLE QVariantMap portAt(qreal x, qreal y, qreal radius) const;
    // The topmost node under the point, or empty.
    Q_INVOKABLE QString nodeAt(qreal x, qreal y) const;
    // Every node that lies wholly inside the rectangle.
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

    // --- nodes ---

    // A new node of an installed type at the point; its id, or empty for an unknown type.
    Q_INVOKABLE QString addNode(const QString &type, qreal x, qreal y);

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
    // An edit changed the document (not a selection or a viewport change).
    void edited();

private:
    friend class NodeList;
    friend class LinkList;

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

    void rebuildLinks();
    void layOut(NodeRow &row) const;
    void autoPlace(const QSet<QString> &unplaced);
    NodeRow makeRow(const QString &id) const;
    void portsFromType(NodeRow &row, const fugue::Node &node) const;
    void refreshPorts(const QString &nodeId); // after a link change: wired flags
    void refreshLinkEnds(int linkRow);
    void insertLink(const LinkRow &link);
    void removeLinksInto(const QString &node, const QString &input, int item);
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
    std::vector<NodeRow> m_rows;
    QHash<QString, int> m_rowOf;
    std::vector<LinkRow> m_links;
    QHash<QString, QList<int>> m_linksOf; // node id -> link rows touching it
    QSet<QString> m_selected;
    QVariantMap m_view;
    qreal m_headerHeight = 26;
    qreal m_rowHeight = 22;

    QVariantMap m_drag;
    QSet<QString> m_dragUnreachable; // nodes the dragged link would make a cycle with

    QStringList m_moving;
    std::vector<QPointF> m_moveStart;
};
