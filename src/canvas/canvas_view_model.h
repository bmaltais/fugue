#pragma once

// The backend of the Canvas view (#221, epics #216 and #217, spec 14.5): the picture of one canvas at a revision you
// choose, built to live in an area of the workspace as well as in the File > Canvas… dialog, so it takes a small
// context (the canvas, a follow target) and knows no dialog. It is CanvasHistory (canvas/canvas_history.h: the
// canvases, revisions, candidates, compare, accept, drop, export) plus what a view of the canvas needs:
//
//  - `choices`: the strip of what to look at, the head first, then the candidates waiting for review, then the
//    few latest revisions; `earlier` is the rest of the history, for a menu. `show(id)` picks one. A candidate
//    is compared with the HEAD (what accepting it would change), whatever it was made from.
//  - an overlay: with a candidate chosen, `overlay` puts it over the head picture; off, the head shows alone.
//    `pictureSource` and `pictureSize` are what the viewer draws, either way.
//  - a follow target: `followNode` is a Canvas Region node (read through WidgetEdit, as the region picker does) whose
//    rectangle `region` describes in canvas pixels, with the padded context box and which revision the node reads;
//    `useForRegion()` writes the shown revision to the node's `revision` as one undo step. Setting the target
//    moves the view to the node's canvas.
//  - auto-refresh: the store's folders are watched, so a head moved by a run, a candidate that arrived or a
//    canvas made elsewhere shows within a moment; a view on the head moves along with it.
//  - new results (#225): a candidate that arrives while the view is on the head is shown over it at once (`newResult`
//    names it, `newResultOthers` counts the ones that came with it or after it, `newResultFrom` says who made it), so a
//    run's output is seen without a click. Never taken from the person: only a view on the head (or on the result it
//    showed itself) moves, and not while `viewLocked` (Compare on, a rectangle drag: the host says) or with
//    `showNewResults` off (settings.json's canvas_show_new_results); a result that came meanwhile is shown when the
//    lock lifts, if the view is still on the head. The banner's Accept and Drop are accept() and drop().
//  - layers (#226): `layers` are the shown revision's layers, top first, as {position (0 is the bottom), name, kind,
//    visible, opacity, blend (an id of blendModes, or "" for an op Fugue does not draw), compositeOp, hasMask, maskEnabled,
//    producer, from (the producer as a person reads it), created, depth (0: groups are later), thumbnail, maskThumbnail
//    (small PNGs of the layer's own pixels and of its mask)}. `selection` is a list of positions (ctrl/shift add, as an
//    outliner does), kept across an edit. Every edit (setVisible, setOpacity, setBlend, rename, removeLayers, duplicate,
//    moveLayers, raise/lower, setMaskEnabled, removeMask) is CanvasStore::editLayers on the shown revision when it is
//    the head (`layersEditable`): ONE new revision each, producer `canvas-view:<op>`, the view moving on to it. undo()
//    and redo() move the head along its parents (CanvasStore::undo, redo). `solo` shows one layer alone: a view-only
//    state, never a revision, dropped when the shown revision changes.
// It makes no window. The `region` map is {valid, reason, x, y, width, height, context: {x, y, width, height}, reads,
// readsId, readsShown, writable, editable, adopts, padding, scale, cap, multiple}: `editable` is false while X, Y, Width or
// Height is linked to another node (the rectangle is then drawn but not edited); `adopts` is true for a node with no
// canvas yet, drawn on the canvas shown, which setRegion and useForRegion write to its `canvas` input in the same undo
// step (#228). Not valid, `reason` says why in plain words and `problem` names it: "notRegion", "linked" (the canvas
// input is linked), "otherCanvas" (with `nodeCanvas`, `known`, `nodeCanvasName`: useCanvasForNode, showNodeCanvas).

#include <QFileSystemWatcher>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

#include "canvas/canvas_history.h"
#include "engine/canvas_layer_ops.h"
#include "canvas/widget_edit.h"

namespace fugue {

class CanvasViewModel : public CanvasHistory {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(WidgetEdit *edits READ edits WRITE setEdits NOTIFY followChanged)
    Q_PROPERTY(QString followNode READ followNode WRITE setFollowNode NOTIFY followChanged)
    Q_PROPERTY(QVariantList choices READ choices NOTIFY choicesChanged)
    Q_PROPERTY(QVariantList earlier READ earlier NOTIFY choicesChanged)
    Q_PROPERTY(bool overlay READ overlay WRITE setOverlay NOTIFY pictureChanged)
    // True when the shown revision is a candidate (open or dropped): the overlay has something to toggle.
    Q_PROPERTY(bool showsCandidate READ showsCandidate NOTIFY compareChanged)
    Q_PROPERTY(QUrl headSource READ headSource NOTIFY pictureChanged)
    Q_PROPERTY(QUrl pictureSource READ pictureSource NOTIFY pictureChanged)
    Q_PROPERTY(QSize pictureSize READ pictureSize NOTIFY pictureChanged)
    Q_PROPERTY(QString head READ head NOTIFY choicesChanged)
    Q_PROPERTY(QVariantMap region READ region NOTIFY regionChanged)
    // Show a candidate that a run produced as soon as it arrives (the setting; the host binds it). On by default.
    Q_PROPERTY(bool showNewResults READ showNewResults WRITE setShowNewResults NOTIFY newResultChanged)
    // True while the view must not move on its own: Compare is on or a rectangle is being dragged. The host binds it.
    Q_PROPERTY(bool viewLocked READ viewLocked WRITE setViewLocked NOTIFY newResultChanged)
    // The candidate the view moved to by itself and still shows ("" otherwise), how many more arrived with it, and who
    // made it: the name of the open document's Canvas Write that wrote it (WidgetEdit::nodeName, its title), else "".
    Q_PROPERTY(QString newResult READ newResult NOTIFY newResultChanged)
    Q_PROPERTY(int newResultOthers READ newResultOthers NOTIFY newResultChanged)
    Q_PROPERTY(QString newResultFrom READ newResultFrom NOTIFY newResultChanged)
    // The layers panel (#226): see the header.
    Q_PROPERTY(QVariantList layers READ layers NOTIFY layersChanged)
    Q_PROPERTY(QVariantList selection READ selection NOTIFY layersChanged)
    Q_PROPERTY(bool layersEditable READ layersEditable NOTIFY layersChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY layersChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY layersChanged)
    Q_PROPERTY(int solo READ solo WRITE setSolo NOTIFY layersChanged)
    // [{id, label}] of the blend modes, Normal first.
    Q_PROPERTY(QVariantList blendModes READ blendModes CONSTANT)

public:
    explicit CanvasViewModel(QObject *parent = nullptr);

    // How many of the latest revisions the strip lists before the rest go to `earlier`.
    static constexpr int recentCount = 5;

    WidgetEdit *edits() const { return m_edits; }
    void setEdits(WidgetEdit *edits);
    QString followNode() const { return m_follow; }
    void setFollowNode(const QString &node);
    QVariantList choices() const { return m_choices; }
    QVariantList earlier() const { return m_earlier; }
    bool overlay() const { return m_overlay; }
    void setOverlay(bool on);
    bool showsCandidate() const;
    QUrl headSource() const { return m_headSource; }
    QUrl pictureSource() const;
    QSize pictureSize() const;
    QString head() const { return m_head; }
    QVariantMap region() const { return m_region; }
    bool showNewResults() const { return m_showNew; }
    void setShowNewResults(bool on);
    bool viewLocked() const { return m_locked; }
    void setViewLocked(bool locked);
    QString newResult() const { return m_newResult; }
    int newResultOthers() const { return m_newOthers; }
    QString newResultFrom() const;
    QVariantList layers() const { return m_layers; }
    QVariantList selection() const;
    bool layersEditable() const;
    bool canUndo() const { return m_canUndo && layersEditable(); }
    bool canRedo() const { return m_canRedo && layersEditable(); }
    int solo() const { return m_solo; }
    void setSolo(int position);
    static QVariantList blendModes();

    // ---- the layers panel (#226); positions are bottom first, as the store has them ----
    // `mode` "only" selects one, "toggle" adds or takes it away (Ctrl), "range" from the last one picked (Shift).
    Q_INVOKABLE void selectLayer(int position, const QString &mode = QStringLiteral("only"));
    Q_INVOKABLE void selectNone();
    // Each is one revision of the head (false when nothing changed or it could not be done; `error` says why).
    Q_INVOKABLE bool setVisible(const QVariantList &positions, bool visible);
    // Hides them all when any is shown, else shows them all (the H key).
    Q_INVOKABLE bool toggleVisible(const QVariantList &positions);
    Q_INVOKABLE bool setOpacity(const QVariantList &positions, double opacity);
    Q_INVOKABLE bool setBlend(const QVariantList &positions, const QString &blend);
    Q_INVOKABLE bool rename(int position, const QString &name);
    Q_INVOKABLE bool removeLayers(const QVariantList &positions);
    Q_INVOKABLE bool duplicate(const QVariantList &positions);
    // `to` is where the lowest of them ends among the others (bottom first); raise and lower move them one place.
    Q_INVOKABLE bool moveLayers(const QVariantList &positions, int to);
    Q_INVOKABLE bool raise(const QVariantList &positions);
    Q_INVOKABLE bool lower(const QVariantList &positions);
    // The drop of a drag in the top-first list: the line above row `gap` (0 is above the top row, the count below the last).
    Q_INVOKABLE bool dropAt(const QVariantList &positions, int gap);
    Q_INVOKABLE bool setMaskEnabled(const QVariantList &positions, bool enabled);
    Q_INVOKABLE bool removeMask(const QVariantList &positions);
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();

    // Looks at a revision: the head, a candidate (against the head) or any other (against its parent).
    Q_INVOKABLE void show(const QString &revision);
    // Writes the shown revision to the followed node's `revision` (the head as "head", so the node keeps reading the
    // head) as one undo step. False when nothing was written: no node, it reads this already, or the input is linked.
    Q_INVOKABLE bool useForRegion();
    // Editing the followed region on the picture (#224). `snapRegion` is the rectangle with its edges on the node's
    // Multiple of (`keepSize`: only the top left moves, as a ratio lock or a move needs); `previewRegion` the numbers a
    // rectangle means for the node, on the shown picture: {x, y, width, height, context: {x, y, width, height},
    // modelWidth, modelHeight, capped}, computed by the node's own functions. `setRegion` writes x, y, width and
    // height as ONE undo step (WidgetEdit::setRegion), snapped when `snap`; false when nothing was written (no valid
    // target, a number linked to another node, or the same numbers).
    Q_INVOKABLE QVariantMap snapRegion(const QVariantMap &rect, bool keepSize = false) const;
    Q_INVOKABLE QVariantMap previewRegion(const QVariantMap &rect) const;
    Q_INVOKABLE bool setRegion(const QVariantMap &rect, bool snap = false);
    // When the followed node reads another canvas (`region.problem` "otherCanvas"): its `canvas` input becomes the one
    // shown (one undo step), or the view goes to the node's canvas (when this root has it).
    Q_INVOKABLE bool useCanvasForNode();
    Q_INVOKABLE void showNodeCanvas();
    // Reads the store again now (the watch does this on its own; a test and a person's refresh ask).
    Q_INVOKABLE void refresh();

signals:
    void followChanged();
    void choicesChanged();
    void pictureChanged();
    void regionChanged();
    void newResultChanged();
    void layersChanged();

private:
    // Stores the edit on the shown head and moves the view to it, with `select` as the selection there.
    bool applyEdit(const canvas::LayerEdit &edit, const QList<int> &select);
    void rebuildLayers();
    void updateSoloPicture();
    QList<int> positionsOf(const QVariantList &list) const;
    void showArrivals();
    void clearNewResult();

    void rebuild();
    void updateRegion();
    void watch();

    WidgetEdit *m_edits = nullptr;
    QString m_follow;
    bool m_overlay = true;
    QVariantList m_choices, m_earlier;
    QUrl m_headSource;
    QSize m_headSize;
    QVariantMap m_region;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
    QMetaObject::Connection m_undoLink;
    bool m_showNew = true;
    bool m_locked = false;
    QStringList m_known;         // the open candidates seen so far on this canvas
    QString m_knownCanvas;       // the canvas m_known belongs to ("" before the first read)
    QStringList m_arrived;       // candidates that came and were not shown yet, newest first
    QString m_newResult;
    int m_newOthers = 0;
    int m_retries = 0;
    QVariantList m_layers;
    QList<int> m_selected;   // positions, in the order picked
    int m_anchor = -1;       // the last one picked, for a range
    QString m_layersOf;      // the revision m_layers is of
    QString m_layersCanvas;  // and its canvas
    QList<int> m_keepSelection; // what an edit selects in the revision it makes
    bool m_editing = false;
    bool m_canUndo = false;
    bool m_canRedo = false;
    int m_solo = -1;
    QUrl m_soloSource;
};

} // namespace fugue
