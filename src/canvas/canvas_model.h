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
// - #48 widgets (done): WidgetEdit (canvas/widget_edit.h), owned here as `widgets`, edits an
//   input's value from the widget on the node face; each port map carries `widgetType` and
//   the stored `literal` for it. A resource input is a value row (the model picker). When a
//   run starts, #19 calls widgets->advanceSeeds() for the seeds set to increment.
// - #19 run state (done; run/canvas_runs.h fills it): setRunState(node, {...}) fills the
//   NodeList roles a run shows (runState, progress, seconds, error, warnings, thumbnail, output
//   values); setRunProgress() changes the progress alone; clearRunState() empties them. With no
//   run they are empty and a node shows no state. previewSize()/previewSizeChanged() are the
//   seam for node sizing (#49): the last image's pixel size, signalled after the re-layout.
//   A node's row is found by id in O(1) (rowOf).
// - #47 look (docs/canvas-look.md): the geometry here is the spec's. u = textSize / 3; a node is
//   60u wide by default, its header 7u, then 1u of padding, outputs first and then inputs in
//   their order, one 6u row each (an unlinked multiline input 16u), a row saying so on a node
//   whose type is not installed, 2u under the last row, and a thumbnail, when the run gave
//   one, 1u under the rows. Wires are cubics with horizontal
//   tangents (8.1); linkAt() finds the wire under the pointer.
// - The window's commands (edit.*, node.bypass, node.mute, node.collapse, view.*) call the
//   edits below; canvas_commands.h registers them.
// - Text size: FugueCanvas binds textSize to the theme's; rows and titles follow it.

#include "canvas/canvas_gesture.h"
#include "canvas/group_edit.h"
#include "canvas/widget_edit.h"
#include "engine/document.h"
#include "engine/node_type.h"
#include "engine/port_type.h"
#include "search/node_search.h"

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <functional>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace fugue {
class ImageInputs;
class Registry;
}

class CanvasModel;
class QUndoStack;

// One row per node, in drawing order (later rows on top).
class NodeList : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")

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
        Color,     // layout color: a theme role (4.5), or empty
        Pinned,    // layout: pinned in place
        // Set by a run (#19, setRunState); empty or -1 when there is none.
        RunState,  // "", "cached", "stale", "running", "frozen", "error" or "skipped" (10.3)
        Progress,  // 0 to 1 while running, else -1
        Seconds,   // time taken, else -1
        Error,     // the error message
        Warnings,  // list of warning messages
        Thumbnail, // the last image or video frame, as a URL
        ThumbnailInfo, // its info chip ("1024 × 1024")
        ThumbnailFile, // the file a Save Image wrote, shown beside the size
        ThumbnailRect, // where it sits in the node, from the node's top left
        ThumbnailText, // text in the thumbnail's place: a shown text or number, a label ("Video · 0:04")
        ThumbnailDim,  // drawn at half strength: a placeholder ("No result yet"), or the last run's
                       // picture while the node runs again, until this run's preview comes
        Compare,       // a Compare Images node's result: {a, b, difference, composite: {url, source, width, height}, labelA, labelB}, else empty
        CompareView,   // its viewer's state (compareView), session only
        Media,         // a video or sound result's player: {type, url, label, width, height, duration, file}, else empty
        Notice,        // why the node cannot run now and how to fix it (8.5), from a check
        UpdateAvailable, // a group whose library version is newer: that version (5.3), else empty
        GroupSize,       // a group instance: how many nodes its definition holds (its caption row), else 0
        NewerMajor,      // a node running on an older installed major: the newest version it can move to (3.6, #118), else empty
        Stack,           // its place in the stacking order, session only: a node that was selected or moved last is the highest (the node's z)
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
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")

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
        Muted, // the source node is muted: its outgoing wires fade (docs/canvas-look.md 9)
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
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(NodeList *nodes READ nodes CONSTANT)
    // The one node the canvas's magnifier shows (#74), as a model of the same rows: the node
    // setLensNode() names, or none.
    Q_PROPERTY(QAbstractItemModel *lensNodes READ lensNodes CONSTANT)
    Q_PROPERTY(LinkList *links READ links CONSTANT)
    Q_PROPERTY(QVariantList frames READ frames NOTIFY framesChanged)
    // Comments ({id, text, x, y, width}) and reroute points ({x, y, color}) from the layout.
    Q_PROPERTY(QVariantList comments READ comments NOTIFY framesChanged)
    Q_PROPERTY(QVariantList reroutes READ reroutes NOTIFY reroutesChanged)
    Q_PROPERTY(QVariantMap view READ view NOTIFY documentReset)
    // Node search (src/search/node_search.h): it adds nodes through addNode() and connectPorts().
    Q_PROPERTY(fugue::NodeSearch *search READ search CONSTANT)
    // Editing input values in place (canvas/widget_edit.h), for the widgets on the node face.
    Q_PROPERTY(WidgetEdit *widgets READ widgets CONSTANT)
    // Group scope and interface editing (canvas/group_edit.h): enter and leave a group, Make
    // group, the exposed inputs and outputs, the library.
    Q_PROPERTY(GroupEdit *groups READ groups CONSTANT)
    // What a press, a drag and a release on the canvas do (canvas/canvas_gesture.h).
    Q_PROPERTY(CanvasGesture *gesture READ gesture CONSTANT)
    // The base text size in px (theme.textSize); a node's title and rows are sized from it.
    Q_PROPERTY(int textSize READ textSize WRITE setTextSize NOTIFY metricsChanged)
    Q_PROPERTY(qreal headerHeight READ headerHeight NOTIFY metricsChanged)
    Q_PROPERTY(qreal rowHeight READ rowHeight NOTIFY metricsChanged)
    Q_PROPERTY(qreal unit READ unit NOTIFY metricsChanged)
    // While a link is dragged: the port it started from ({node, port, output, color, x, y}),
    // else empty. Ports bind their dimming to this, so it changes at the start and end only.
    Q_PROPERTY(QVariantMap linkDrag READ linkDrag NOTIFY linkDragChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY undoChanged)
    Q_PROPERTY(QVariant pointer READ pointer WRITE setPointer)
    // `L` (view.toggle-links): the wires are hidden; the view's choice, not an edit.
    Q_PROPERTY(bool linksHidden READ linksHidden WRITE setLinksHidden NOTIFY linksHiddenChanged)
    // The selected node ids, in drawing order.
    Q_PROPERTY(QStringList selection READ selectedIds NOTIFY selectionChanged)
    // The group levels entered (spec 5.2): [{id, title}] from the outside in; empty at the top.
    Q_PROPERTY(QVariantList breadcrumb READ breadcrumb NOTIFY scopeChanged)
    Q_PROPERTY(bool inGroup READ inGroup NOTIFY scopeChanged)

public:
    explicit CanvasModel(const fugue::Registry *registry, QObject *parent = nullptr);
    ~CanvasModel() override;

    NodeList *nodes() const { return m_nodeList; }
    QAbstractItemModel *lensNodes() const { return m_lensNodes; }
    Q_INVOKABLE void setLensNode(const QString &node);
    LinkList *links() const { return m_linkList; }
    fugue::NodeSearch *search() const { return m_search; }
    WidgetEdit *widgets() const { return m_widgets; }
    GroupEdit *groups() const { return m_groups; }
    CanvasGesture *gesture() const { return m_gesture; }
    // Pasting and dropping images (workflow/image_inputs.h): set by Workflows; the paste command
    // and the Paste button of Load Image go through it. Null without one.
    fugue::ImageInputs *images() const { return m_images; }
    void setImages(fugue::ImageInputs *images) { m_images = images; }

    // The registry was filled again (#70, engine/registry_reloader.h): every node is made again
    // from the types it has now, so a placeholder whose type came shows as the node, a node whose
    // pack went as a placeholder, and a library group newer than the workflow's shows "update
    // available"; node search lists the new types. Nothing of the document changes: no undo step.
    void refreshNodeTypes();

    // Starts the workflow at its top level, with an empty undo stack.
    void setDocument(const fugue::Document &document);
    // The whole workflow, with the canvas's positions and viewport in its layout: what is
    // saved and run. While a group is entered, the level on screen is in its definition.
    fugue::Document document() const;
    // The level on screen (spec 5.2), with its positions: the whole workflow at the top; inside
    // a group, a document whose graph.nodes are the group's inner nodes and whose layout is the
    // group's layout.groups entry, with every definition in `groups`. Every edit starts from
    // it and hands it to commit(), which puts it back where it belongs.
    fugue::Document scopeDocument() const;

    // --- groups (spec 5) ---
    //
    // Entering a group shows its definition's inside; editing it edits the definition, so every
    // instance changes (5.3). Entering and leaving are not edits, like the viewport; an undo
    // restores the whole workflow and the canvas stays on its level, or goes up to the nearest
    // level that is still there. canvas/group_edit.h has the group edits (`groups`); the scope itself is kept here.

    // The instances entered, outside in ({id, title}), for the breadcrumb; empty at the top.
    QVariantList breadcrumb() const;
    bool inGroup() const { return !m_scope.isEmpty(); }
    QStringList scope() const { return m_scope; }
    // The level's path id prefix (4.8): the instance ids joined with '/', empty at the top.
    QString scopePath() const { return m_scope.join(QLatin1Char('/')); }
    // The id of the group whose inside is shown, empty at the top.
    QString scopeGroup() const;
    // Whether an instance of this group of the workflow may be added to the level shown (not
    // when the level is inside it, 5.2).
    bool canAddGroup(const QString &group) const { return canHoldGroup(m_document, group); }
    // Whether the node of the level is a group instance.
    Q_INVOKABLE bool isGroup(const QString &node) const;
    // How many nodes in the whole workflow (inside groups too) are instances of the group `node`
    // is one of; 0 when it is not a group.
    int groupInstances(const QString &node) const;
    // Frame the selection (5.1, Ctrl+G): a Frame around the selected nodes, holding them.
    Q_INVOKABLE QString frameSelection();
    // Moving a Frame moves the nodes it holds with it (5.1): beginFrameMove, then moveBy and
    // endMove as for nodes.
    Q_INVOKABLE void beginFrameMove(const QString &frame);
    // The topmost Frame whose title bar (its top `band` world units) holds the point.
    Q_INVOKABLE QString frameAt(qreal x, qreal y, qreal band) const;
    Q_INVOKABLE void renameFrame(const QString &frame, const QString &title);
    Q_INVOKABLE void deleteFrame(const QString &frame);

    // The version the library has of the node's group when it is newer than the workflow's
    // definition ("update available"), else empty.
    QString updateAvailable(const QString &node) const;
    // A node that runs on an older major of its type while a newer one is installed (3.6, #90):
    // the newest version it can move to, else empty. Never a group instance (it has its update).
    Q_INVOKABLE QString newerMajor(const QString &node) const;
    // What moving the node would change: {name, from, to, addedInputs, removedInputs,
    // addedOutputs, removedOutputs}; empty when there is no newer major.
    Q_INVOKABLE QVariantMap majorMove(const QString &node) const;
    // Rewrites the node's version to the newest one (values and links stay, the compiler flags
    // what the new version lacks). One undo step.
    Q_INVOKABLE bool moveToNewerMajor(const QString &node);
    // The node.move-version command: nodeMoveAsked(majorMove(selected)) for the window to show.
    Q_INVOKABLE void requestMajorMove();
    QVariantList frames() const;
    QVariantList comments() const;
    QVariantList reroutes() const;
    // {center: [x, y], zoom} from layout.view, or empty when the file has none.
    QVariantMap view() const;
    // Records the view the canvas shows (the canvas calls it as it pans and zooms).
    Q_INVOKABLE void setView(qreal centerX, qreal centerY, qreal zoom);
    // Moves the canvas's view there: setView, and viewMoveRequested for the canvas to follow.
    void moveView(qreal centerX, qreal centerY, qreal zoom);

    int textSize() const { return m_textSize; }
    // Lays every node and wire out again: one reset of each list.
    void setTextSize(int size);
    qreal headerHeight() const { return m_headerHeight; }
    qreal rowHeight() const { return m_rowHeight; }
    qreal unit() const { return m_textSize / 3.0; }
    // A node's width when the layout sets none: 60u (longer text values widen it, up to the maximum).
    qreal defaultWidth() const { return m_defaultWidth; }
    // What a manual resize allows (docs/canvas-look.md 4.1): 40u to 120u wide.
    qreal minWidth() const { return qRound(40 * unit()); }
    qreal maxWidth() const { return qRound(120 * unit()); }

    // --- hit tests, in world coordinates ---

    // The port within `radius` of the point, nearest first: {node, port, output, x, y, shape}
    // (x and y: its center), or empty.
    Q_INVOKABLE QVariantMap portAt(qreal x, qreal y, qreal radius) const;
    // The topmost node under the point, or empty.
    Q_INVOKABLE QString nodeAt(qreal x, qreal y) const;
    // Every node the rectangle touches, as ComfyUI's marquee selects.
    Q_INVOKABLE QStringList nodesIn(const QRectF &rect) const;
    // The wire within `radius` of the point, nearest first: {row, fromNode, fromPort, toNode,
    // toPort, item, midX, midY} (mid: where its cut handle goes), or empty. Only wires whose
    // bounds hold the point are measured, each in 16 straight pieces per curve.
    Q_INVOKABLE QVariantMap linkAt(qreal x, qreal y, qreal radius) const;

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
    // cannot reach is worked out once when it starts. Styled, it is StyledText for the reason
    // chip: the type names bold (docs/canvas-look.md 8.2), everything else escaped.
    Q_INVOKABLE QString dragProblem(const QString &node, const QString &port, bool output,
                                    bool styled = false) const;
    // A drag let go on a node's body (#74): the ports of `node` it can end on, free ones first
    // (an input with no link; any output), as [{port, label, output, free}]. Empty for the node
    // the drag comes from, or with no drag.
    Q_INVOKABLE QVariantList dropTargets(const QString &node) const;
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
    // Its seed widgets start as setNewNodeOptions says, and so does a Save node's embed.
    Q_INVOKABLE QString addNode(const QString &type, qreal x, qreal y);
    // What new nodes start with (settings.json's new_seed and embed_workflow): seed widgets
    // on "random" (randomize) or "fixed" (a random number kept), and an `embed` choice input
    // (Save Image, 11.7) on that choice when the input offers it.
    void setNewNodeOptions(const QString &seed, const QString &embed);
    // Removes the selected nodes and every link into or out of them.
    Q_INVOKABLE void deleteSelection();
    // Bypass or mute (3.5) the selection, `mode` being "bypass" or "mute": every selected
    // node takes the mode, unless all of them have it already, and then all go back to normal.
    Q_INVOKABLE void toggleMode(const QString &mode);
    // Collapses the selection, or expands it when all of it is collapsed already.
    Q_INVOKABLE void toggleCollapsed();
    // The node.pin command (#55): pins the selection in place, or unpins it when all are pinned;
    // a pinned node does not move with a drag or its Frame. One undo step.
    Q_INVOKABLE void togglePinned();
    // The node.align-* and node.distribute-* commands (#58): `mode` is "left", "right", "top" or
    // "bottom" (the selection shares that edge) or "horizontal" or "vertical" (equal gaps between
    // the nodes; three or more). A pinned node stays. One undo step; false when nothing moved.
    Q_INVOKABLE bool alignSelection(const QString &mode);
    // The edit.rename command (#55): the title the canvas shows, layout.nodes.<id>.title; empty
    // goes back to the type's name. One undo step.
    // On a group card it renames the group (#74, as ComfyUI renames a subgraph from its node's
    // title), and when the group is in the library offers to rename it there (libraryRenameOffered).
    Q_INVOKABLE void renameNode(const QString &node, const QString &title);
    // True when the selection is not empty and every selected node has the state the toggle
    // commands act on: "bypass", "mute", "collapsed" or "pinned" (what they would remove).
    bool selectionAll(const QString &state) const;

    // --- run state (#19) ---

    // What a run shows on a node, replacing what it showed: {state, progress, seconds, error,
    // warnings, thumbnail, thumbnailAspect (height / width), thumbnailInfo, values (output
    // name -> the last value as text)}. Missing keys are empty. Kept across edits of the node.
    Q_INVOKABLE void setRunState(const QString &node, const QVariantMap &state);
    Q_INVOKABLE void clearRunState();
    // Only a running node's progress (0 to 1): one role, no layout, for a run's frequent updates.
    void setRunProgress(const QString &node, qreal progress);
    // The node's title as the canvas shows it; empty when there is no such node.
    Q_INVOKABLE QString nodeTitle(const QString &node) const;
    // The node as a sentence names it (#228): its title, with its id after it only when another node shown has the same
    // title ("Canvas Region (12)"); empty when there is no such node. User text names a node so, never by its id alone.
    Q_INVOKABLE QString nodeName(const QString &node) const;

    // A Compare Images node's viewer (#62): what a person changed on it, kept here so the node's
    // face, the large view and the keys share one state. It is session state, like a node's
    // last result: no undo step, no unsaved mark, not in the workflow file. Keys: mode
    // ("slider", "slider_horizontal", "side_by_side", "flip", "difference"), position (0..1),
    // showB, zoom (1 = fit), actual (1:1), centerX, centerY. Unset keys start from the node's own
    // Mode and Position widgets.
    Q_INVOKABLE bool isCompare(const QString &node) const;
    Q_INVOKABLE QVariantMap compareView(const QString &node) const;
    // Its last result's pictures (the `compare` role): a, b, difference, composite and the labels.
    Q_INVOKABLE QVariantMap compareResult(const QString &node) const { return m_run.value(node).compare; }
    // A video or sound result's player data (the `media` role): what the large view plays.
    Q_INVOKABLE QVariantMap mediaResult(const QString &node) const { return m_run.value(node).media; }
    Q_INVOKABLE void setCompareView(const QString &node, const QVariantMap &changes);
    // One of "flip", "next-mode", "fit", "actual" on the node, as the keys do. False when the
    // node is no Compare Images or the step is unknown.
    bool compareStep(const QString &node, const QString &step);
    // The one selected node when it is a Compare Images, else empty.
    QString selectedCompare() const;
signals:
    void compareViewChanged(const QString &node);
public:
    // The Crop node's frame (#73): its x, y, width and height as the node cuts them (the values the
    // node holds, the defaults for one never set), {x, y, width, height, editable}; `editable` is false
    // when any of the four is linked, whose value is not known before a run. Empty for another node.
    Q_INVOKABLE bool isCrop(const QString &node) const;
    Q_INVOKABLE QVariantMap cropRect(const QString &node) const;
    // Writes the four numbers as one undo step (named "Crop"), only those that changed; false when
    // nothing changed, the node is no Crop or one of the four is linked.
    Q_INVOKABLE bool setCropRect(const QString &node, const QVariantMap &rect);
    // The painted mask of a node (spec 14.4, #174): the node's first input of widget `mask`, empty for
    // a node with none. `maskState` is {input, over, path, editable}: the input's name, the image
    // input it is painted over ("" for the node's own file, as Load Image), the stored literal as it
    // is (empty for none) and whether it can be changed (not linked). Empty without such an input.
    Q_INVOKABLE QString maskInput(const QString &node) const;
    Q_INVOKABLE QVariantMap maskState(const QString &node) const;
    // Sets the mask input to the file `path` (empty: no mask) as one undo step; the problem as text, or
    // empty. The mask editor's `saved(path)` goes here.
    Q_INVOKABLE QString setMaskPath(const QString &node, const QString &path);
    // The painted canvas of a node (spec 14.4, #181): the node's first input of widget `paint`, as the
    // mask above. `paintState` is {input, over, path, editable, linked, width, height}: `over` is the
    // picture input, `linked` whether a node feeds it (its picture is known only after a run), and
    // `width` and `height` the node's Width and Height when they are plain numbers (0 for unset or linked),
    // the size of a blank canvas. Empty without such an input.
    Q_INVOKABLE QString paintInput(const QString &node) const;
    Q_INVOKABLE QVariantMap paintState(const QString &node) const;
    // Sets the paint input to the file `path` (empty: nothing painted) as one undo step; the problem as
    // text, or empty. The paint window's `saved(path)` goes here.
    Q_INVOKABLE QString setPaintPath(const QString &node, const QString &path);
    // The world point (x, y) lies on the node's picture (a result or a file's image), not on a text
    // standing in its place: where a double click opens the large view.
    Q_INVOKABLE bool pictureAt(const QString &node, qreal x, qreal y) const;
    // The node whose output feeds the node's input, or empty (a literal, nothing, or a missing node).
    Q_INVOKABLE QString linkedFrom(const QString &node, const QString &input) const;
    // The seam for #49 (node size): the pixel size of the node's last image (setRunState's
    // `thumbnailSize`), empty without one. previewSizeChanged(node) fires when a thumbnail
    // comes, goes or changes size, after the node's rows were laid out again with it.
    Q_INVOKABLE QSizeF previewSize(const QString &node) const;
    // The node card made its rows (it came into view at a readable zoom): rowsMade(node) tells
    // whoever makes previews only for those (CanvasRuns, the image of a Load Image's file).
    Q_INVOKABLE void noteRowsMade(const QString &node) { emit rowsMade(node); }

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
    // An edit that begins while a mouse gesture (a move, a link drag, a resize) is open ends
    // that gesture first, so the gesture's step holds the gesture alone.
    void beginEdit();
    void endEdit(const QString &text);
    // Makes the document this one as one undo step named `text`, touching only what differs
    // (every edit here ends in it; a reload from outside, 11.8, can use it too). Start from
    // document(), which has the positions on screen.
    void commit(const fugue::Document &document, const QString &text);
    // Rewrites the document on the canvas and every document the undo steps keep, as no step
    // of its own: for a change that names the same things another way, such as relative file
    // paths made absolute before a Save As to another folder (workflows.cpp), so an undo
    // afterwards still names the files it named before.
    void rewriteDocuments(const std::function<void(fugue::Document &)> &rewrite);
    bool canUndo() const;
    bool canRedo() const;
    QString undoText() const;
    QString redoText() const;
    // Steps kept per stack; the oldest go first.
    static constexpr int undoLimit = 50;
    // A gesture still in progress is finished first.
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // --- view commands ---

    // Sets the view to fit the selection, or every node when none is selected, in a canvas
    // of this size (ComfyUI's `.`). False when there are no nodes.
    Q_INVOKABLE bool fitView(qreal width, qreal height, qreal reservedTop = 0, qreal reservedBottom = 0);
    // Ask the canvas to zoom by the factor at its center, or to fit (fitRequested).
    void requestZoom(qreal factor) { emit zoomRequested(factor); }
    void requestFit() { emit fitRequested(); }
    bool linksHidden() const { return m_linksHidden; }
    void setLinksHidden(bool hidden) {
        if (hidden != m_linksHidden) {
            m_linksHidden = hidden;
            emit linksHiddenChanged();
        }
    }
    static constexpr qreal minZoom = 0.1;
    static constexpr qreal maxZoom = 4;

    // Moving: beginMove picks what moves (the selection when the node is in it, else just
    // the node), moveBy offsets them from where they started, endMove finishes.
    // Resizing (#49). A node is auto-sized unless the layout pins its size: it grows to fit
    // its content (longer text values, multiline text, the run's thumbnail) and is
    // recomputed only when that changes. A resize pins the width and height (never below
    // what the content needs, 40u to 120u wide); autoSize() unpins them. Any corner of an
    // open node resizes it (#63): resizeCornerAt finds the node and corner ("nw", "ne",
    // "sw", "se") whose square of `size` world units, inside the node, holds the point, or
    // empty. A top or left corner moves the node too, so the opposite corner stays put. A
    // resize, begin to end, is one undo step.
    Q_INVOKABLE QVariantMap resizeCornerAt(qreal x, qreal y, qreal size) const;
    Q_INVOKABLE void beginResize(const QString &node, const QString &corner = QStringLiteral("se"));
    Q_INVOKABLE void resizeBy(qreal dx, qreal dy); // the corner's travel since beginResize
    Q_INVOKABLE void endResize();
    Q_INVOKABLE bool autoSize(const QString &node);
    // The hook for anything that changes what a node's content needs without a document
    // edit (a run's thumbnail arriving, #19): lays the node out again, tells the views what
    // moved (size, thumbnail, ports) and brings its wires along. False when nothing changed.
    Q_INVOKABLE bool refreshSize(const QString &node);

    // The node whose Advanced section header (5.4, 6.1) holds the point, or empty; and showing
    // or folding that section (layout only, one undo step).
    Q_INVOKABLE QString advancedHeaderAt(qreal x, qreal y) const;
    Q_INVOKABLE void toggleAdvanced(const QString &node);

    Q_INVOKABLE void beginMove(const QString &node);
    Q_INVOKABLE void moveBy(qreal dx, qreal dy);
    Q_INVOKABLE void endMove();
    Q_INVOKABLE QPointF position(const QString &node) const;
    // The node's box in the world, as drawn (a collapsed one is its header); empty for none.
    Q_INVOKABLE QRectF nodeRect(const QString &node) const;

    // --- selection ---

    // mode: "replace", "add", "subtract" or "toggle".
    Q_INVOKABLE void select(const QStringList &ids, const QString &mode = QStringLiteral("replace"));
    Q_INVOKABLE QStringList selectedIds() const;
    // The link rows into or out of these nodes (the wires a selection highlights).
    Q_INVOKABLE QList<int> linkRows(const QStringList &nodes) const;
    Q_INVOKABLE void selectAll();

    // A port type's color and shape (12.6). The color is a key for the theme's portColor()
    // (a built-in type's id, or "declared"), or "any" or "unknown" (a placeholder's port);
    // the QML picks the color. The shape: circle, square, diamond or ring.
    static QString colorKey(const std::optional<fugue::PortSpec> &type, fugue::PortKind kind);
    static QString shape(const std::optional<fugue::PortSpec> &type, fugue::PortKind kind);

signals:
    void documentReset();
    // setDocument() put another document in (a tab, a file opened); documentReset follows. Not
    // sent when a group is entered or left, or by an undo.
    void documentReplaced();
    // The level shown changed: a group was entered or left (the rows were reset just before).
    void scopeChanged();
    void nodeMoveAsked(const QString &node, const QVariantMap &changes);
    // The group.interface command: the window opens the interface editor of the group shown.
    void interfaceEditorRequested();
    // The frame.rename command: the window asks for the title.
    void frameRenameRequested(const QString &frame);
    // The node.color.custom command (#52): the window opens the color wheel for the Frame
    // `frame`, or for the selected nodes when it is empty.
    void colorPickRequested(const QString &frame);
    // The edit.rename command and a double-click on a title (#55) or on a group's port (#72): the
    // window asks for the new title or label.
    void nodeRenameRequested(const QString &node);
    void portRenameRequested(const QString &node, const QString &port, bool output);
    // A paste added nodes from this document: the same questions as for opening it (missing
    // nodes, packs to trust) are asked of it (Workflows).
    void pasted(const fugue::Document &document);
    void framesChanged();
    void reroutesChanged();
    void metricsChanged();
    void linkDragChanged();
    void selectionChanged();
    void linksHiddenChanged();
    // An edit changed the document (not a selection or a viewport change), undo and redo
    // included.
    void edited();
    void undoChanged();
    void previewSizeChanged(const QString &node);
    void rowsMade(const QString &node);
    void zoomRequested(qreal factor);
    void fitRequested();
    void viewMoveRequested();

private:
    // The first input of a node whose widget is `widget` (mask_edit.cpp: the mask and paint windows).
    QString widgetInput(const QString &node, const QString &widget) const;
    // {input, over, path, editable} of that input, empty without one.
    QVariantMap fileWidgetState(const QString &node, const QString &widget) const;
    void toggleLayoutFlag(const QString &flag, const QString &text);
    // Starts moving these nodes: all but the pinned ones (#55), which are returned.
    QStringList startMoving(const QStringList &ids);
    friend class NodeList;
    friend class LinkList;
    friend class EditCommand;
    friend class WidgetEdit;
    friend class GroupEdit;

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
        bool widget = false; // a value input: its row shows its widget when unlinked
        QString widgetType;  // its 6.2 widget, or "model" for a resource input (2.5)
        QString role;        // "mask" on a mask port (6.1, 14.4): drawn as a hollow square
        QString over;        // widget `mask`: the image input it is painted over (6.1)
        bool required = false;
        QJsonValue defaultValue = QJsonValue::Undefined;
        std::optional<double> min, max;
        qreal y = 0;         // the port's center, from the node's top
        qreal rowTop = 0;    // its row
        qreal rowHeight = 0;
        // Sections (6.1, 5.4): an input starting a section has its header row above it, at
        // headerTop; an advanced input stays folded (no row) until the node's Advanced section
        // is open, unless it is linked.
        QString section;
        bool advanced = false;
        qreal headerTop = -1; // -1: no header above it
        QString header;       // the header's section name
        bool hidden = false;
    };
    // Stacking order of nodes that overlap: raised when a node is selected or moved, not saved.
    QHash<QString, int> m_stack;
    int m_stackTop = 0;
    void raise(const QString &node);

    struct NodeRow {
        QString id;
        QString title;
        bool missing = false;
        QPointF pos;
        qreal width = 0;
        bool sized = false; // the layout pins the width (a manual resize, layout.nodes.<id>.size)
        qreal pinnedHeight = 0; // the height the layout pins, 0: automatic (never below the content's)
        // The tallest content the pinned height has stood over since it was pinned or read: when
        // the content is shorter again and nothing can take the room, the room was only spare
        // and the node is its content's height (Appendix A). Not saved.
        qreal pinnedOver = 0;
        qreal height = 0;
        bool collapsed = false;
        qreal collapsedWidth = 0; // the header alone: fits the title (docs/canvas-look.md 9)
        // What the canvas shows and hit tests: the width, or the collapsed header's.
        qreal shownWidth() const { return collapsed ? collapsedWidth : width; }
        QString color;      // layout color
        bool pinned = false;
        bool advancedOpen = false; // layout: the Advanced section shown (layout.nodes.<id>.advanced)
        int groupSize = 0;         // a group instance: its definition's node count, shown on a row of its own
        QRectF thumbnail;   // where the run's thumbnail goes, empty without one
        std::vector<Port> inputs;
        std::vector<Port> outputs;
        // A group instance's exposed inputs as the group narrows them (compiler.h's
        // exposedInputSpec), what its widgets offer and check.
        std::map<QString, fugue::InputSpec> exposed;
    };
    struct LinkRow {
        fugue::PortRef from;
        QString toNode;
        QString toInput;
        int item = -1;
        QString color;
        QPointF a, b;
        QString path;
        std::vector<std::array<QPointF, 4>> curves; // the path's cubics
        QRectF bounds;
    };
    struct RunInfo {
        QString state;
        qreal progress = -1;
        qreal seconds = -1;
        QString error;
        QStringList warnings;
        QString thumbnail;
        qreal thumbnailAspect = 1;
        QSizeF thumbnailSize;
        QString thumbnailInfo;
        QString thumbnailFile;
        QString thumbnailText;
        bool thumbnailDim = false;
        QVariantMap compare;
        QVariantMap media;
        QVariantMap values;
        QString notice;
    };

    Snapshot snapshot() const;
    // The level `scope` of the whole workflow `root` (scopeDocument's form), or nothing when the
    // scope does not lead through group instances of it.
    static std::optional<fugue::Document> levelOf(const fugue::Document &root, const QStringList &scope);
    // The whole workflow with this level (of the current scope) put back into its definition;
    // the group's face is pruned of what the level no longer has.
    fugue::Document merged(const fugue::Document &level) const;
    // Shows a level: the rows and links made again from it (setDocument without the undo), at
    // `view` (else the level's saved view, else none).
    void loadLevel(const fugue::Document &level, const QVariantMap *view = nullptr);
    // Commits a change of the whole workflow (a face edit, which instances elsewhere follow).
    void commitWhole(const fugue::Document &whole, const QString &text);
    // An instance of `group` may go in the level shown: false when it would hold itself (5.2).
    bool canHoldGroup(const fugue::Document &document, const QString &group) const;
    // Makes the document this one: only nodes that differ are touched, and their links.
    // With selectAdded, nodes that come back are selected (undo of a delete, redo of a paste).
    void restore(const Snapshot &target, bool selectAdded);
    void relink(const QSet<QString> &nodes);
    void finishGestures();
    // Ends the open mouse gestures, each as its own step (beginEdit's rule).
    void endMouseGestures();
    void contentChanged();
    static std::unique_ptr<QUndoStack> makeUndoStack();
    void watchUndoStack();
    // Adds a copy's nodes with new ids, its top left at topLeft, as one edit; selects them.
    QStringList insertCopy(const fugue::Document &copy, QPointF topLeft, bool connected, const QString &text,
                           QHash<QString, QString> *idMap);
    fugue::Document copyOf(const QStringList &ids) const;
    QRectF boundsOf(const QStringList &ids) const;

    void rebuildLinks();
    void layOut(NodeRow &row) const;
    qreal autoWidth(const NodeRow &row, const fugue::Node &node) const;
    // layOut again and emit what changed (plus `roles`); the node's wires follow.
    // `resized`: the caller already changed the size state, so nothing before it to compare with.
    bool relayOut(int row, QList<int> roles = {}, bool resized = false);
    void refreshLinksOf(const QString &node);
    std::pair<qreal, qreal> pinnedSize(const QJsonObject &entry) const;
    void autoPlace(const QSet<QString> &unplaced);
    NodeRow makeRow(const QString &id) const;
    void portsFromType(NodeRow &row, const fugue::Node &node) const;
    void refreshPorts(const QString &nodeId); // after a link change: wired flags
    void refreshLinkEnds(int linkRow);
    void emitNodeChanged(int row, const QList<int> &roles);
    QVariantList portMaps(const NodeRow &row, bool outputs) const;
    QVariantMap portMap(const NodeRow &row, const Port &port) const;
    const Port *findPort(const QString &node, const QString &port, bool output) const;
    // The spec of a node's input: its type's, or for a group instance the inner input an
    // exposed input leads to. None for a placeholder or an unknown name.
    const fugue::InputSpec *inputSpec(const QString &node, const QString &input) const;
    QString problem(const QString &fromNode, const QString &output, const QString &toNode, const QString &input,
                    bool useDrag, bool styled = false) const;
    QSet<QString> upstreamOf(const QString &node) const;
    QSet<QString> downstreamOf(const QString &node) const;
    bool isWired(const QString &node, const QString &port, bool output) const;
    QString displayValue(const fugue::InputValue &value, const Port &port, bool *isDefault) const;
    int rowOf(const QString &node) const { return m_rowOf.value(node, -1); }

    const fugue::Registry *m_registry;
    QString m_newSeed = QStringLiteral("random");
    QString m_newEmbed = QStringLiteral("full");
    fugue::Document m_document;
    NodeList *m_nodeList;
    LinkList *m_linkList;
    fugue::NodeSearch *m_search;
    WidgetEdit *m_widgets;
    GroupEdit *m_groups;
    CanvasGesture *m_gesture;
    fugue::ImageInputs *m_images = nullptr;
    std::vector<NodeRow> m_rows;
    QHash<QString, int> m_rowOf;
    std::vector<LinkRow> m_links;
    void reindexLinks(); // m_linksOf from m_links
    QHash<QString, QList<int>> m_linksOf; // node id -> link rows touching it
    QSet<QString> m_selected;
    QVariantMap m_view;
    int m_textSize = 12;
    qreal m_headerHeight = 28;  // 7u, rounded
    qreal m_rowHeight = 24;     // 6u
    qreal m_tallRowHeight = 64; // 16u: an unlinked multiline input
    qreal m_padTop = 4;         // 1u
    qreal m_padBottom = 8;      // 2u
    qreal m_defaultWidth = 240; // 60u
    QHash<QString, RunInfo> m_run;
    QHash<QString, QVariantMap> m_compareView; // a Compare Images node's viewer state, never saved

    QVariantMap m_drag;
    QSet<QString> m_dragUnreachable; // nodes the dragged link would make a cycle with

    QStringList m_moving;
    QStringList m_pinnedMembers; // a Frame move's pinned nodes: they stay, and stay its members (#55)
    std::vector<QPointF> m_moveStart;
    bool m_moveOpen = false;  // a move holds an edit open
    bool m_cloneOpen = false; // an Alt+drag clone holds one more
    bool m_dragOpen = false;  // a link drag holds an edit open
    bool m_resizeOpen = false; // a resize holds one too
    bool m_gestureEditing = false; // a gesture makes its own edits (a link put down, a clone)
    QString m_resizeNode;
    bool m_resizeWest = false, m_resizeNorth = false; // the corner dragged
    QRectF m_resizeStart;     // its box when the resize began
    bool m_resizeWasSized = false;
    qreal m_resizeWasWidth = 0, m_resizeWasHeight = 0;

    // The whole workflow while a group is entered (its entered level is stale: m_document has
    // it); m_scope the instances entered, m_outerViews the view of each level left behind.
    fugue::Document m_root;
    QStringList m_scope;
    std::vector<QVariantMap> m_outerViews;
    QSortFilterProxyModel *m_lensNodes = nullptr;
    QStringList m_movingFrames; // Frames a move carries (beginFrameMove)
    std::vector<QRectF> m_frameStart;

    std::unique_ptr<QUndoStack> m_undo;
    int m_editDepth = 0;
    Snapshot m_editBefore;
    std::optional<QPointF> m_pointer;
    bool m_linksHidden = false;
};
