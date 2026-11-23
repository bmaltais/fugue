#pragma once

// The backend of the Canvas view (#221, epics #216 and #217, spec 14.5): the picture of one canvas at a revision you
// choose, built to live in an area of the workspace as well as in the File > Canvas… dialog, so it takes a small
// context (the canvas, a follow target) and knows no dialog. It is CanvasHistory (canvas/canvas_history.h: the
// canvases, revisions, candidates, compare, accept, drop, export) plus what a view of the canvas needs:
//
//  - `choices`: the head first, then the few latest revisions; `earlier` is the rest of the history, for a menu.
//    `show(id)` picks one. `pictureSource` and `pictureSize` are what the viewer draws.
//  - a follow target: `followNode` is a Canvas Region node (read through WidgetEdit, as the region picker does) whose
//    rectangle `region` describes in canvas pixels, with the padded context box and which revision the node reads;
//    `useForRegion()` writes the shown revision to the node's `revision` as one undo step. Setting the target
//    moves the view to the node's canvas.
//  - auto-refresh: the store's folders are watched, so a head moved by a run, a candidate that arrived or a
//    canvas made elsewhere shows within a moment; a view on the head moves along with it.
//  - a run lands on the head (Canvas Write, #228, #239: candidate review is no part of the product): the view on the head
//    goes along and `headNotice` says what came, with Undo.
//  - layers (#226): `layers` are the shown revision's layers, top first, as {position (0 is the bottom), name, kind,
//    visible, opacity, blend (an id of blendModes, or "" for an op Fugue does not draw), compositeOp, hasMask, maskEnabled,
//    producer, from (the producer as a person reads it), created, depth (0: a group is one row, its children inside it are not listed; kind "group"), thumbnail, maskThumbnail
//    (small PNGs of the layer's own pixels and of its mask)}. `selection` is a list of positions (ctrl/shift add, as an
//    outliner does), kept across an edit. Every edit (setVisible, setOpacity, setBlend, rename, removeLayers, duplicate,
//    moveLayers, raise/lower, setMaskEnabled, removeMask) is CanvasStore::editLayers on the shown revision when it is
//    the head (`layersEditable`): ONE new revision each, producer `canvas-view:<op>`, the view moving on to it. undo()
//    and redo() move the head along its parents (CanvasStore::undo, redo). `solo` shows one layer alone: a view-only
//    state, never a revision, dropped when the shown revision changes.
//  - the mask brush (#230): `maskTarget` is the position of the layer whose layer mask the view's brush paints (-1: none),
//    set by targetMask() (a click on the mask's thumbnail) or addMask() (a new, fully revealing mask: one revision, and
//    the brush then targets it, as Photoshop does). The mask is painted into `maskLayer` (the view's MaskLayer item); each
//    maskPainted() (a stroke or shape ended) restarts a short idle (`maskIdle` ms), and when it runs out the strokes made
//    meanwhile are stored as ONE revision (CanvasStore::setLayerMask, producer `canvas-view:mask-paint`; its unchanged tiles
//    shared). Anything that needs the head first (undo, redo, another layer, another tool) commits at once
//    (commitMask). invertMask, fillMask and clearMask are one revision each. Undo goes back one revision and the mask
//    is read again from it. useSelection() stores a selection painted in the view (Quick Mask) by content and writes its
//    hash and bounds into the followed Canvas Region's `selection` input as one undo step (Region 1.2).
// It makes no window. The `region` map is {valid, reason, x, y, width, height, context: {x, y, width, height}, reads,
// readsId, readsShown, writable, editable, active, pinned, padding, scale, cap, multiple}: `editable` is false while X, Y, Width or
// Height is linked to another node (the rectangle is then drawn but not edited); `active` is true for a node with no
// canvas chosen (it reads the Active canvas, #246: edits here leave its `canvas` empty), `pinned` for one that names a canvas. Not valid, `reason` says why in plain words and `problem` names it: "notRegion", "linked" (the canvas
// input is linked), "otherCanvas" (with `nodeCanvas`, `known`, `nodeCanvasName`: useCanvasForNode, showNodeCanvas).

#include <QFileSystemWatcher>
#include <QPointer>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

#include "canvas/canvas_history.h"
#include "canvas/mask_layer.h"
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
    Q_PROPERTY(QUrl headSource READ headSource NOTIFY pictureChanged)
    Q_PROPERTY(QUrl pictureSource READ pictureSource NOTIFY pictureChanged)
    Q_PROPERTY(QSize pictureSize READ pictureSize NOTIFY pictureChanged)
    Q_PROPERTY(QString head READ head NOTIFY choicesChanged)
    // The canvas's history as a person reads it (#228, the owner's decision: Photoshop's History panel, no hashes): the
    // Head's revisions newest first, each {id, name, time, timeText, kind ("head" or "history"), latest}, `name` a step
    // in words from what changed against its parent ("Layer added: A wooden table", "Opacity 50%: Fill", "Layer
    // deleted: Sketch", "Mask added: Fill", "Painted: Base", "Canvas made"), whoever made it (a run, this view, the
    // command line). `allSteps` adds the candidates the command line made, open and dropped ("Candidate: …", "Dropped: …"),
    // first, for `Show all revisions` (#239: candidate review is no part of the view; they are only listed).
    Q_PROPERTY(QVariantList steps READ steps NOTIFY choicesChanged)
    Q_PROPERTY(QVariantList allSteps READ allSteps NOTIFY choicesChanged)
    Q_PROPERTY(QVariantMap region READ region NOTIFY regionChanged)
    // The Head moved by itself to a run's result while the view was on the Head (a Canvas Write that lands on the Head,
    // the normal flow since #228): the view went along, and this says what came ("Layer Fill 1 added"), for a short
    // notice with Undo; "" otherwise. It ends when anything else is shown (an undo included) or clearHeadNotice().
    Q_PROPERTY(QString headNotice READ headNotice NOTIFY headNoticeChanged)
    // The layers panel (#226): see the header.
    Q_PROPERTY(QVariantList layers READ layers NOTIFY layersChanged)
    Q_PROPERTY(QVariantList selection READ selection NOTIFY layersChanged)
    Q_PROPERTY(bool layersEditable READ layersEditable NOTIFY layersChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY layersChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY layersChanged)
    Q_PROPERTY(int solo READ solo WRITE setSolo NOTIFY layersChanged)
    // The mask brush (#230): see the header.
    Q_PROPERTY(fugue::MaskLayer *maskLayer READ maskLayer WRITE setMaskLayer NOTIFY maskChanged)
    Q_PROPERTY(int maskTarget READ maskTarget NOTIFY maskChanged)
    Q_PROPERTY(bool maskPending READ maskPending NOTIFY maskChanged)
    Q_PROPERTY(int maskIdle READ maskIdle WRITE setMaskIdle NOTIFY maskChanged)
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
    QVariantList steps() const { return m_steps; }
    QVariantList allSteps() const { return m_allSteps; }
    // The name of the step a revision is (see `steps`); read once per revision (they never change).
    Q_INVOKABLE QString stepName(const QString &revision);
    QUrl headSource() const { return m_headSource; }
    QUrl pictureSource() const;
    QSize pictureSize() const;
    QString head() const { return m_head; }
    QVariantMap region() const { return m_region; }
    QString headNotice() const { return m_headNotice; }
    Q_INVOKABLE void clearHeadNotice();
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
    // #252, the Layer menu: each is one revision of the head. newLayer puts an empty paint layer above the current one (on top
    // when none is) and selects it; mergeDown draws the layer at `position` into the one under it (selected after); flatten
    // makes every shown layer one; groupLayers puts the layers into a new group (selected); ungroup takes the children of the
    // groups out (selected). setProperties applies what a Layer properties dialog changed to one layer as ONE revision: any of
    // `name`, `opacity` (0 to 1), `blend` (an id of blendModes), `maskEnabled` and `removeMask` (true), those that differ.
    Q_INVOKABLE bool newLayer();
    Q_INVOKABLE bool mergeDown(int position);
    Q_INVOKABLE bool flatten();
    Q_INVOKABLE bool groupLayers(const QVariantList &positions);
    Q_INVOKABLE bool ungroup(const QVariantList &positions);
    Q_INVOKABLE bool setProperties(int position, const QVariantMap &changes);
    // #236, Edit > Paste as new layer and a picture file dropped on the view: the picture (untrusted: canvas/picture_import.h) goes on the
    // Head as a new layer right above the current one (on top when none), centered on (`cx`, `cy`) canvas pixels (a negative
    // `cx` or `cy`: the canvas's center), named "Pasted N" (a file: its name). It is kept whole as the layer's original, so a
    // picture bigger than the canvas can be scaled down later. ONE revision, producer `canvas-view:paste`, history "Layer
    // pasted: <name>". False, with `error` in words, when nothing could be placed (no picture, too large, not the Head).
    Q_INVOKABLE bool pasteLayer(double cx = -1, double cy = -1);
    Q_INVOKABLE bool placeFile(const QString &pathOrUrl, double cx = -1, double cy = -1);
    // Whether a dragged file is one placeFile reads (a drop takes only pictures; the window's own drop keeps the rest).
    Q_INVOKABLE bool isPictureFile(const QString &pathOrUrl) const;
    // Empties the canvas (File > Clear canvas): ONE new revision of the Head, the same size, one empty layer; Undo takes it
    // back. False when the Head is not what is shown.
    Q_INVOKABLE bool clearCanvas();
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();

    // ---- the mask brush (#230) ----
    MaskLayer *maskLayer() const { return m_maskLayer; }
    void setMaskLayer(MaskLayer *layer);
    int maskTarget() const { return m_maskTarget; }
    bool maskPending() const { return m_maskDirty; }
    int maskIdle() const { return m_maskIdle.interval(); }
    void setMaskIdle(int ms);
    // A fully revealing (white) layer mask on the layer, one revision of the head; the brush then targets it. A layer that
    // has one already is only targeted. False when nothing could be done (not the head, a mask layer).
    Q_INVOKABLE bool addMask(int position);
    // The brush paints this layer's mask (selects the layer alone and reads its mask into `maskLayer`); false when it has none.
    Q_INVOKABLE bool targetMask(int position);
    // The brush paints no layer mask any more (what was painted is stored first).
    Q_INVOKABLE void untargetMask();
    // A stroke or shape ended on `maskLayer`: the idle starts again.
    Q_INVOKABLE void maskPainted();
    // Stores what was painted since the last revision now, as one revision (`what` names it: canvas-view:<what>).
    Q_INVOKABLE bool commitMask(const QString &what = QStringLiteral("mask-paint"));
    Q_INVOKABLE bool invertMask();
    Q_INVOKABLE bool fillMask();
    Q_INVOKABLE bool clearMask();
    // The selection painted in `selection` (a MaskLayer, Quick Mask) for the followed Canvas Region: stored by content and
    // written to its `selection` input with its bounds, one undo step. {ok, hash, reason}: `reason` says in words why not.
    Q_INVOKABLE QVariantMap useSelection(fugue::MaskLayer *selection);

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
    // A pinned node goes back to the Active canvas: its `canvas` input is emptied (one undo step).
    Q_INVOKABLE bool useActiveCanvasForNode();
    // Reads the store again now (the watch does this on its own; a test and a person's refresh ask).
    Q_INVOKABLE void refresh();

signals:
    void followChanged();
    void choicesChanged();
    void pictureChanged();
    void regionChanged();
    void headNoticeChanged();
    void layersChanged();
    void maskChanged();

private:
    // Stores the edit on the shown head and moves the view to it, with `select` as the selection there.
    bool applyEdit(const canvas::LayerEdit &edit, const QList<int> &select);
    // A picture placed as a layer (pasteLayer, placeFile).
    bool placePicture(const mask::MaskPaint &picture, double cx, double cy, const QString &name);
    // A stored edit: the view moves on to it (false, with `error`, when it did not land).
    bool land(const Result<canvas::Commit> &done, const QList<int> &select);
    // The target's mask read again from the shown revision when it is another one (an undo, a run's layer).
    void syncMask();
    bool loadMask(int position);
    void rebuildLayers();
    void updateSoloPicture();
    QList<int> positionsOf(const QVariantList &list) const;

    void rebuild();
    void updateRegion();
    void watch();

    WidgetEdit *m_edits = nullptr;
    QString m_follow;
    QVariantList m_choices, m_earlier;
    QVariantList m_steps, m_allSteps;
    QHash<QString, QString> m_stepNames; // revision id -> its step's name
    QUrl m_headSource;
    QSize m_headSize;
    QVariantMap m_region;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
    QMetaObject::Connection m_undoLink;
    QString m_headNotice;
    QString m_noticeHead; // the Head the notice is about
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
    QPointer<MaskLayer> m_maskLayer;
    int m_maskTarget = -1;
    QString m_maskOf; // the revision whose mask `m_maskLayer` holds
    bool m_maskDirty = false;
    QTimer m_maskIdle;
};

} // namespace fugue
