pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import Fugue
import "../menu"
import "widgets"
import "canvasMenu.js" as CanvasMenu

// The Canvas view (#221, epics #216 and #217, spec 14.5): one canvas as a picture you can look at, choose a revision of,
// decide on and read a region from. It is a self-contained item: it needs no dialog, area or window around it, only
// its context (below), so the same item sits in the File > Canvas… dialog and in an area of the workspace.
//
// Context: `canvasId` (empty: the newest canvas of the store, or the one a followed node reads), `storeRoot` (empty is
// the session's), `edits` (the document's WidgetEdit) and `selection` (the Graph's). The follow target, `followNode`, is
// a Canvas Region node whose rectangle is then drawn on the picture, EDITED there (#224) and which **Use for region**
// writes to; the view finds it without a hunt in the graph (#228): the selected Region node; else the one last followed
// or chosen, while it reads the shown canvas; else the only Region node that reads it. With none of these a drag on the
// picture still draws a rectangle (the "free" selection, view-only): with several Region nodes on this canvas it asks
// which one it is for (a menu by their titles, remembered), with none it offers **Add a Canvas Region node** (one undo
// step: the node, reading this canvas, with the rectangle); until then nothing keeps it.
//
// The region on the picture is a live frame (CropFrame, the Crop window's and the region picker's): drag it to move it,
// its eight handles to resize it, or drag in empty space to draw a new one. The numbers, the padded context box and the
// size the model gets follow the pointer; the release writes x, y, width and height as ONE undo step (no Apply), Escape
// during a drag leaves the node as it was. Wheel zoom and the middle button (or Space held) pan and never touch the
// rectangle. It edits whichever revision is shown; with X, Y, Width or Height linked to another
// node, or while comparing, it is drawn and not edited.
// Everything it does is CanvasViewModel (canvas/canvas_view_model.h) over CanvasHistory.
//
// The layers panel (#226, CanvasLayersPanel.qml) is at the right of the picture: the shown revision's layers, each edit one
// revision of the Head, Undo and Redo of the canvas; **Layers** in the tools shows or hides it. `ownShortcuts` is for a
// host that is a modal popup (the dialog): the panel then binds its keys itself.
//
// Simple by default (#228, the owner's decision: Photoshop's History, no strip, no hashes): the canvas, the picture
// (wheel zoom, Fit, 100%), the Layers panel with Undo and Redo, and **History** (closed at first): the canvas's changes
// in words, newest first; a click shows that step (read only) and **Back to latest** returns; **Show all revisions**
// adds every revision for those who need them (film teams; the ones the command line made as candidates too).
// Compare (against the step before) opens the compare modes of Compare Images (CompareModeButtons.qml). One bar (#228):
// the canvas, then the tools in chunks (History and Layers, the look, Export, Fit and 100%) and the host's `trailing` items
// last (the dialog's close). A run's layer lands on the latest version and is followed, with a short notice "Layer
// <name> added" and Undo (#239: there is no candidate review; a run is a layer, delete it or Undo).
// Keys (docs/ui-ergonomics.md D5, one keymap with the Graph): Home or 0 Fit, 1 100%, + and - zoom, Escape ends a drag, Space held pans. While it has the
// focus the window's bare Q (quit) does not fire here: it is kept for a quick mask (docs/ui-ergonomics.md References);
// Ctrl+Q and the close button still quit, and the Graph's keys sleep while a Canvas area is the active one (Workspace).
// Right to left the bar, the notes and the layers panel mirror with the window; the picture never does.
//
// The tool strip (#230; Photoshop's toolbar, Blender's toolbar: docs/ui-ergonomics.md References) is a narrow column at
// the start of the picture, one icon button per tool with its key in the tooltip, the current one in the quiet marking:
// Hand (H; Space held for a moment), Zoom (Z; Alt zooms out), Move (V, #236: drags the current layer; Ctrl+T is Free Transform), Rectangle region (M or R: the frame above, the default),
// Mask brush (B) and eraser (E; X swaps them), then Quick Mask (Q) at its foot. The options bar over the picture holds the
// current tool's settings: Snap for the region; Zoom in or out; for the brush what it paints (the mask of a layer, the
// Quick Mask selection, or nothing yet: Add mask, Quick Mask), Size, Hardness and Opacity as drag-number fields ([ and ]
// size it, Alt+drag too), the mask's overlay (\, red over the hidden part) and its menu (Invert, Ctrl+I; Reveal all,
// Hide all), Quick Mask's Use selection and Clear selection. The tools are commands of the registry (`canvas-view.*`, the
// `canvas-view` context, on while the pointer is over the view or it has the focus); the layers panel's own keys win over
// it. A layer mask is painted into `layerMask` (a MaskLayer): the brush hides (the painted part is the hidden part, in
// red) and the eraser shows, and the strokes are ONE revision of the Head once the brush rests (CanvasViewModel's mask
// brush); Ctrl+Z then takes the canvas back a step. Quick Mask paints the selection into `selectionMask` (view-only,
// undone stroke by stroke); Use selection stores it by content and hands it to the followed Canvas Region's `selection`
// input (Region 1.2, one undo step), so the next run redraws the painted part and lands as a layer.
Item {
    id: root
    objectName: "canvasView"
    required property CanvasLook look
    property alias storeRoot: canvasData.storeRoot
    property alias canvasId: canvasData.canvasId
    property alias edits: canvasData.edits
    // The Graph's selection (node ids): a Canvas Region among it is the follow target.
    property var selection: []
    readonly property string followNode: canvasData.followNode
    // The Region node last followed or chosen for a drawn rectangle: it stays the target while it reads this canvas.
    property string chosenRegion: ""
    // The document's Canvas Region nodes [{id, name, canvas}], and those that read the shown canvas.
    readonly property var regionNodes: canvasData.edits && canvasData.edits.graphRevision >= 0 ? canvasData.edits.regionNodes() : []
    // A node with no canvas chosen reads the Active canvas, which this view reports as its own when it is used (#246).
    readonly property var regionsHere: regionNodes.filter(node => node.canvas === canvasData.canvasId || node.canvas === "")
    readonly property string selectedRegion: selection.length === 1 && regionNodes.some(node => node.id === selection[0]) ? selection[0] : ""
    onSelectedRegionChanged: if (selectedRegion !== "") chosenRegion = selectedRegion
    readonly property string followTarget: selectedRegion !== "" ? selectedRegion
                                         : regionsHere.some(node => node.id === chosenRegion) ? chosenRegion
                                         : regionsHere.length === 1 ? regionsHere[0].id : ""
    // A rectangle drawn while no node is followed ({width: 0}: none yet); view-only until a node takes it.
    readonly property var noRect: ({x: 0, y: 0, width: 0, height: 0})
    property var freeRect: noRect
    readonly property bool freeMode: canvasData.followNode === "" && pictureView.visible && canvasData.pictureSize.width > 0
    readonly property bool hasFreeRect: freeMode && freeRect.width > 0
    // The Fit and 100% buttons at the end of the tools; a host can turn them off.
    property bool zoomButtons: true
    // The host's items at the very end of the tools row: the dialog's close (one bar, as the Mask editor's).
    property alias trailing: trailingChunk.data
    // The layers panel at the right (#226), and whether it binds its own keys (a modal host's window shortcuts sleep).
    property bool showLayers: true
    // The width of the History and Layers panels: 56u in a roomy view, a quarter of a narrow one (a small Canvas area), never
    // under 32u, so the picture keeps most of the width.
    readonly property real sideWidth: Math.max(32 * u, Math.min(56 * u, width / 4))
    property bool ownShortcuts: false
    readonly property alias layersPanel: layersPanel
    readonly property alias backend: canvasData
    readonly property alias viewer: pictureView
    readonly property real u: look.unit
    readonly property bool mirrored: Application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.enabled: mirrored
    LayoutMirroring.childrenInherit: true
    property bool comparing: false
    // The rectangle a drag shows now (canvas pixels), null at rest; Snap puts its edges on the node's Multiple of.
    property var liveRect: null
    property bool snap: false
    // Rulers, Grid, Guides and Snap to them (#253): view options of this window, not part of the canvas. The guides themselves and
    // the grid's spacing are the canvas's (`guidesData`, a sidecar file in the canvas's folder).
    property bool showRulers: false
    property bool showGrid: false
    property bool showGuides: true
    property bool snapToLines: true
    property bool panKey: false // Space held: a left drag pans, as the middle button does
    // Why the last New canvas… or Import picture… made nothing (the error line says it).
    property string makeProblem: ""
    readonly property bool regionValid: canvasData.region.valid === true
    // What the snap pulls to now (GuidesModel.snapRect's options): lines only when they are in view, so nothing hidden pulls.
    readonly property var snapOptions: ({
        multiple: root.snap && regionValid ? (canvasData.region.multiple ?? 0) : 0,
        guides: root.snapToLines && root.showGuides && overlaysShown && guidesData.guides.length > 0,
        grid: root.snapToLines && root.showGrid && overlaysShown && gridLines.drawn,
        zoom: pictureView.imageScale})
    // The overlays (rulers, grid, guides) mean pixels of one picture: not while two are compared or a difference is shown.
    readonly property bool overlaysShown: pictureView.visible && !compareOn && !pictureView.paired && pictureView.mode !== "difference"
    readonly property real rulerSize: 5 * u
    readonly property bool regionEditable: regionValid && canvasData.region.editable === true && !compareOn
    // What the rectangle means for the node (the node's own functions): the drag's, or the committed one.
    // The preview needs the picture's size (previewRegion answers nothing before it is read); reading pictureSize here
    // makes the binding run again when the picture arrives, whichever came first: the region or the picture. `previewReady`
    // is derived from the preview itself, so no binding can see "ready" before the data (QML runs bindings in no fixed order).
    readonly property var preview: regionValid && canvasData.pictureSize.width > 0 && canvasData.pictureSize.height > 0
                                   ? canvasData.previewRegion(liveRect ?? canvasData.region) : ({})
    readonly property bool previewReady: preview.context !== undefined
    // The Canvas Region reads a step that is not the latest version (a pinned Revision input: rare): the one case its
    // note and Use for region are worth the room (#234).
    readonly property bool pinnedElsewhere: {
        const reads = canvasData.region.reads ?? ""
        if (reads === "" || reads === "head")
            return false
        const latest = canvasData.revisions.find(r => r.latest)
        return !latest || canvasData.region.readsId !== latest.id
    }
    // The selection the status line says: the drag's, the node's rectangle, or a drawn one.
    // The Canvas Region node's rectangle, or the free one; none while the node's numbers are all 0 (it reads the whole canvas
    // and the view shows no selection: Select > Deselect leaves it so).
    readonly property bool regionSet: regionValid && canvasData.region.unset !== true
    readonly property var shownRegion: regionSet ? canvasData.region : regionValid ? noRect : freeRect
    readonly property var selectionRect: liveRect ?? (regionSet ? canvasData.region : !regionValid && hasFreeRect ? freeRect : null)
    property bool advancedDetails: false
    readonly property bool compareOn: comparing && canvasData.hasParent
    readonly property bool shownIsHead: canvasData.revisionB !== "" && canvasData.revisionB === canvasData.head
    // The revisions shown, as CanvasHistory names them ({name, tag, kind, ...}; {} before any).
    readonly property var rowB: canvasData.revisions.find(row => row.id === canvasData.revisionB) ?? ({})
    readonly property var rowA: canvasData.revisions.find(row => row.id === canvasData.revisionA) ?? ({})
    // The followed node as a sentence names it: its title (WidgetEdit::nodeName), never its id alone.
    readonly property string followName: canvasData.followNode === "" ? "" : (canvasData.edits ? canvasData.edits.nodeName(canvasData.followNode) : "")
                                                                             || canvasData.followNode

    // The index in the model's list of a guide row of the view's filtered list (guides off the canvas are not drawn).
    function guideIndex(row) { return guidesData.guides.findIndex(g => g.axis === row.axis && g.at === row.at) }
    function fit() { pictureView.fit() }
    function actualSize() { pictureView.actualSize() }
    // Reads the store again now (a host shows it again after it was away).
    function refresh() { canvasData.refresh() }
    // What a person calls the revision shown (no hashes, #228): the latest version or the step it is.
    function labelOf(row) {
        if (row.kind === undefined)
            return ""
        return row.kind === "head" ? qsTr("Latest") : canvasData.stepName(row.id)
    }
    property bool showHistory: false

    // ---- the tool strip and its options (#230) ----
    property string tool: "region" // hand, zoom, move, region, lasso, brush, eraser
    property bool quickMask: false
    property bool zoomOut: false
    property real brushSize: 64
    property real brushHardness: 0.8
    property real brushOpacity: 1
    property bool maskOverlay: true
    // Why the last Use selection did nothing (the options bar says it; a stroke clears it).
    property string selectionProblem: ""
    readonly property bool brushing: tool === "brush" || tool === "eraser"
    // The lasso (#230) draws a selection into the same mask Quick Mask paints: Shift adds, Alt subtracts, else it replaces.
    readonly property bool lassoing: tool === "lasso"
    // Ctrl+Z undoes the selection, not the canvas, right after it changed (a lasso, a selection from a layer); a new revision ends it.
    property bool selectionLast: false
    readonly property real maxBrush: Math.max(64, Math.min(2048, Math.max(canvasData.pictureSize.width, canvasData.pictureSize.height)))
    // #252: the rows the layers panel has selected, and whether Merge down has two shown layers that can merge (paint layers or groups).
    readonly property var chosenRows: layersPanel.chosen.map(p => canvasData.layers.find(r => r.position === p)).filter(r => r !== undefined)
    // #236: the current layer can be moved, flipped or turned: a single paint layer (not a group, mask or picture) on the latest version.
    readonly property bool transformable: canvasData.layersEditable && layersPanel.current !== null && chosenRows.length === 1 && layersPanel.current.kind === "paint" && layersPanel.current.locked !== true
    readonly property bool mergeable: {
        const upper = layersPanel.current
        if (!canvasData.layersEditable || upper === null || chosenRows.length !== 1 || upper.position < 1)
            return false
        const lower = canvasData.layers.find(r => r.position === upper.position - 1)
        const merges = r => r !== undefined && r.visible && !r.locked && r.kind !== "mask" && r.kind !== "picture"
        return merges(upper) && merges(lower)
    }
    readonly property bool paintsMask: !quickMask && canvasData.maskTarget >= 0
    readonly property var maskRow: canvasData.layers.find(r => r.position === canvasData.maskTarget) ?? null
    readonly property var paintTarget: quickMask || lassoing ? selectionLayer : paintsMask ? layerMask : null
    readonly property alias layerMaskItem: layerMask
    readonly property alias selectionMask: selectionLayer
    // The view's keys are live (the `canvas-view` context): the pointer is over it, or its picture has the focus.
    // A menu of the view's own bar is open: the pointer is over the popup, not the view, yet the commands are the view's.
    property bool menuOpen: false
    // The host closes the view itself (the dialog): Close canvas asks it with `closeRequested`; an area just shows no canvas.
    property bool hostCloses: false
    signal closeRequested()
    readonly property bool keysHot: viewHover.hovered || pictureView.activeFocus || menuOpen
    onKeysHotChanged: App.commands.setContextActive("canvas-view", keysHot)
    function keysOf(id) { return (App.commands.entry(id).keys ?? []).join(", ") }
    function setTool(name) {
        toolBeforeQuickMask = "" // a tool picked by hand stays when Quick Mask ends
        if (name === tool)
            return
        if (freeTransform)
            commitTransform() // another tool keeps the transform, as Photoshop does
        canvasData.commitMask() // the strokes so far are their own step
        tool = name
    }
    // Move and Free Transform (#236): the layer's placement being made. The Move tool drags the current layer (one revision when the
    // button is let go); Free Transform (Ctrl+T) opens the box with its handles and the numbers, and Enter keeps it (one revision),
    // Escape leaves it. Nothing is stored until then: the picture shows the canvas without the layer and the frame draws the layer's
    // original at `xfPlacement` (CanvasViewModel.beginTransform), whose numbers are TransformGesture's.
    property var xfPlacement: null
    property var xfStart: null
    property int xfPosition: -1
    property bool freeTransform: false
    property bool xfLink: false
    readonly property bool xfOpen: xfPosition >= 0
    function beginTransformSession() {
        if (xfOpen)
            return true
        const row = layersPanel.current
        if (!transformable || !row) {
            canvasData.showNotice(row && row.kind !== "paint" ? qsTr("Only a paint layer can be moved or transformed.") : qsTr("Pick one paint layer to move or transform."))
            return false
        }
        if (!canvasData.beginTransform(row.position))
            return false
        const box = canvasData.layerBox(row.position)
        if (!box.valid) {
            canvasData.endTransform()
            return false
        }
        xfPosition = row.position
        xfStart = xfGesture.describe(box)
        xfPlacement = xfStart
        return true
    }
    function endTransformSession() {
        canvasData.endTransform()
        xfPlacement = null
        xfStart = null
        xfPosition = -1
        freeTransform = false
    }
    function startFreeTransform() {
        if (freeTransform || !beginTransformSession())
            return
        freeTransform = true
        pictureView.forceActiveFocus()
    }
    // Keeps what the box shows as ONE revision; a box nobody changed makes none.
    function commitTransform() {
        if (!xfOpen)
            return
        const changed = JSON.stringify(xfPlacement) !== JSON.stringify(xfStart)
        const position = xfPosition
        const placement = xfPlacement
        endTransformSession()
        if (changed)
            canvasData.transformLayer(position, placement)
    }
    function nudgeLayer(dx, dy) {
        if (freeTransform)
            xfPlacement = xfGesture.nudged(xfPlacement, dx, dy)
        else if (tool === "move" && transformable)
            canvasData.moveLayerBy(layersPanel.current.position, dx, dy)
    }
    function setSize(v) { brushSize = Math.max(1, Math.min(maxBrush, Math.round(v))) }
    function setHardness(v) { brushHardness = Math.max(0, Math.min(1, Math.round(v * 100) / 100)) }
    function stepSize(up) { setSize(up ? Math.max(brushSize + 1, brushSize * 1.25) : Math.min(brushSize - 1, brushSize / 1.25)) }
    // Quick Mask takes the brush, and leaving it gives back the tool it took the place of.
    property string toolBeforeQuickMask: ""
    function toggleQuickMask() {
        canvasData.commitMask()
        quickMask = !quickMask
        selectionProblem = ""
        if (quickMask && !brushing) {
            toolBeforeQuickMask = tool
            tool = "brush"
        } else if (!quickMask && toolBeforeQuickMask !== "" && brushing) {
            tool = toolBeforeQuickMask
        }
        if (!quickMask)
            toolBeforeQuickMask = ""
    }
    // The brush goes to a layer mask just targeted (Add mask, a click on a mask's thumbnail), as Photoshop's does.
    function paintTheMask() {
        quickMask = false
        if (!brushing)
            tool = "brush"
    }
    function addMaskToCurrent() {
        const row = root.layersPanel.current
        if (row && canvasData.addMask(row.position))
            paintTheMask()
    }
    // The selection from a layer's mask or alpha (#231), put in as `how` (replace, add, subtract); a rectangle gives way.
    function selectFromLayer(position, fromMask, how) {
        if (canvasData.selectFromLayer(position, fromMask, selectionLayer, how)) {
            freeRect = noRect
            selectionProblem = ""
            selectionLast = true
        }
    }
    // A lasso ended: it is the selection now (a free rectangle gives way) and Ctrl+Z takes it back.
    function lassoDone() {
        freeRect = noRect
        selectionLast = true
    }
    function useSelection() {
        const answer = canvasData.useSelection(selectionLayer)
        selectionProblem = answer.ok ? "" : answer.reason
    }
    function maskMenu(button) {
        const entries = quickMask
              ? [{text: qsTr("Invert"), value: "invert", detail: keysOf("canvas-view.mask-invert")},
                 {text: qsTr("Select all"), value: "fill"}]
              : [{text: qsTr("Invert"), value: "invert", detail: keysOf("canvas-view.mask-invert")},
                 {text: qsTr("Reveal all"), value: "fill"},
                 {text: qsTr("Hide all"), value: "clear"}]
        openMenu(button, entries, value => {
            if (value === "invert") root.runTool("canvas-view.mask-invert")
            else if (root.quickMask) selectionLayer.fill()
            else if (value === "fill") canvasData.fillMask()
            else canvasData.clearMask()
        }, 40)
    }

    // ---- the selection, as the Select menu knows it (#232 slice B) ----
    // The rectangle (the Region node's, or the free one) has an area; the painted selection has pixels.
    readonly property bool rectSelected: regionValid ? regionSet : hasFreeRect
    readonly property bool maskSelected: !selectionLayer.empty
    // Undo and Redo go to the selection: Quick Mask is on, the lasso is the tool, or the selection just changed.
    readonly property bool selectionUndoes: quickMask || ((lassoing || selectionLast) && (selectionLayer.canUndo || selectionLayer.canRedo))
    // What Deselect can clear now: the free rectangle, the node's area (when its numbers are not linked or compared), the painted selection.
    readonly property bool deselectable: (rectSelected && (!regionValid || regionEditable)) || maskSelected
    // Escape clears the selection when nothing else wants it: no drag, no run of the task bar or the Graph.
    readonly property bool escapeDeselects: deselectable && !freeTransform && !taskBar.busy && !dragging && App.commands.entry("run.cancel").enabled !== true
    // The node's area goes back to 0, 0, 0, 0 (the whole canvas: the run takes the edge, one undo step, a stored selection
    // goes too); the free rectangle and the painted selection are cleared.
    function deselect() {
        selectionProblem = ""
        if (hasFreeRect)
            freeRect = noRect
        if (regionValid && regionEditable && (regionSet || canvasData.region.selection))
            canvasData.setRegion(noRect)
        if (maskSelected)
            selectionLayer.clear()
    }
    // ---- Select > Modify and Transform selection (#251) ----
    // The painted selection is the one the operations work on. A rectangle (the free one, or the Region node's) is painted into
    // it first, and the free rectangle goes (the Region node's area stays: Deselect clears it). The result is shown as the
    // Quick Mask, the only view of a painted selection. Each is one step of the selection's own undo (Ctrl+Z).
    readonly property bool canModifySelection: canvasData.canvasId !== "" && pictureView.visible && (maskSelected || rectSelected)
    function selectionToMask() {
        if (maskSelected)
            return true
        const r = regionValid ? canvasData.region : freeRect
        if (!rectSelected || !r || r.width <= 0 || r.height <= 0)
            return false
        selectionLayer.fillRect(r.x, r.y, r.width, r.height, false)
        if (hasFreeRect)
            freeRect = noRect
        return true
    }
    function showSelectionMask() {
        if (!quickMask)
            toggleQuickMask()
    }
    function modifySelection(kind, amount) {
        const op = {expand: "grow", contract: "shrink", feather: "feather", border: "border", smooth: "smooth"}[kind]
        if (!op || !selectionToMask())
            return
        selectionLayer.modify(op, amount)
        showSelectionMask()
    }
    function transformSelection(dx, dy, scaleX, scaleY, degrees) {
        if (!selectionToMask())
            return
        selectionLayer.transformBy(dx, dy, scaleX, scaleY, degrees)
        showSelectionMask()
    }
    function selectAll() {
        const w = canvasData.pictureSize.width, h = canvasData.pictureSize.height
        if (w <= 0 || h <= 0)
            return
        const whole = {x: 0, y: 0, width: w, height: h}
        if (quickMask)
            selectionLayer.fill()
        else if (regionValid)
            canvasData.setRegion(whole)
        else if (freeMode)
            placeFree(whole)
    }
    function exportDialogOpen() {
        exportDialog.currentFolder = StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        exportDialog.selectedFile = StandardPaths.writableLocation(StandardPaths.PicturesLocation) + "/" + canvasData.exportName
        exportDialog.open()
    }
    // Export with quality and scale… and Export layers as files… (#247, #248): one dialog, made the first time.
    property var exportDialogBox: null
    function exportWith(mode) {
        if (!exportDialogBox)
            exportDialogBox = exportComponent.createObject(root, { storeRoot: canvasData.storeRoot })
        const why = exportDialogBox.openFor(canvasData.canvasId, canvasData.revisionB, mode)
        if (why !== "")
            canvasData.showError(why)
    }
    // Close canvas: the host's close (the dialog), or the area shows no canvas.
    function closeCanvas() {
        if (hostCloses)
            closeRequested()
        else
            canvasData.canvasId = ""
    }
    // What the menu bar's rows say about this view now: {enabled, checkable, checked, keys}; undefined for a row that has no
    // state of its own (a tool's key command, which was never guarded).
    function menuState(id) {
        const has = canvasData.canvasId !== ""
        const shows = pictureView.visible
        const tool = name => ({enabled: has, checkable: true, checked: root.tool === name})
        switch (id) {
        case "canvas-view.new": case "canvas-view.import": return {enabled: true}
        case "canvas-view.open": return {enabled: canvasData.canvases.length > 0}
        case "canvas-view.export": case "canvas-view.export-options": case "canvas-view.export-layers": return {enabled: canvasData.revisionB !== ""}
        case "canvas-view.batch": return {enabled: App.canvasBatch !== null}
        case "canvas-view.rename": case "canvas-view.share": case "canvas-view.duplicate": case "canvas-view.delete": case "canvas-view.close": return {enabled: has}
        case "canvas-view.clear": return {enabled: canvasData.layersEditable}
        case "canvas-view.paste-layer": case "canvas-view.place-picture": return {enabled: canvasData.layersEditable}
        case "canvas-view.paste-canvas": return {enabled: true}
        case "canvas-view.flip-horizontal": case "canvas-view.flip-vertical": case "canvas-view.turn-clockwise": case "canvas-view.turn-counterclockwise":
            return {enabled: transformable}
        case "canvas-view.select-all": return {enabled: shows && canvasData.pictureSize.width > 0}
        case "canvas-view.deselect": return {enabled: deselectable}
        case "canvas-view.invert-selection": return {enabled: quickMask || maskSelected}
        case "canvas-view.selection-expand": case "canvas-view.selection-contract": case "canvas-view.selection-feather":
        case "canvas-view.selection-border": case "canvas-view.selection-smooth": case "canvas-view.selection-transform":
            return {enabled: canModifySelection}
        case "canvas-view.use-selection": return {enabled: quickMask && regionValid && canvasData.region.selectionWritable === true}
        case "canvas-view.quick-mask": return {enabled: has, checkable: true, checked: quickMask}
        case "canvas-view.add-mask": return {enabled: canvasData.layersEditable && layersPanel.current !== null && layersPanel.current.kind !== "mask"}
        case "canvas-view.mask-invert": return {enabled: quickMask || paintsMask}
        case "canvas-view.mask-overlay": return {enabled: paintsMask || compareOn, checkable: true, checked: maskOverlay}
        case "canvas-view.mask-reveal": case "canvas-view.mask-hide": return {enabled: paintsMask}
        // #252: the layer operations, on the layers panel's selection.
        case "canvas-view.new-layer": case "canvas-view.flatten": return {enabled: canvasData.layersEditable && canvasData.layers.length > 0}
        case "canvas-view.group": return {enabled: canvasData.layersEditable && chosenRows.length > 0 && chosenRows.every(r => r.kind !== "picture")}
        case "canvas-view.ungroup": return {enabled: canvasData.layersEditable && chosenRows.length > 0 && chosenRows.every(r => r.kind === "group")}
        case "canvas-view.merge-down": return {enabled: mergeable}
        case "canvas-view.layer-properties": return {enabled: canvasData.layersEditable && layersPanel.current !== null}
        case "canvas-view.undo": return {enabled: !selectionUndoes || selectionLayer.canUndo}
        case "canvas-view.redo": return {enabled: !selectionUndoes || selectionLayer.canRedo}
        // The keys the view handles by hand are shown as text (they are not bound twice).
        case "canvas-view.fit": return {enabled: shows, keys: ["Home"]}
        case "canvas-view.actual-size": return {enabled: shows, keys: ["1"]}
        case "canvas-view.zoom-in": return {enabled: shows, keys: ["+"]}
        case "canvas-view.zoom-out": return {enabled: shows, keys: ["-"]}
        case "canvas-view.history": return {enabled: has, checkable: true, checked: showHistory}
        case "canvas-view.layers": return {enabled: has, checkable: true, checked: showLayers}
        case "canvas-view.compare": return {enabled: canvasData.hasParent, checkable: true, checked: compareOn}
        case "canvas-view.details": return {enabled: has, checkable: true, checked: advancedDetails}
        case "canvas-view.snap": return {enabled: root.tool === "region" && regionEditable && canvasData.region.multiple > 1, checkable: true, checked: snap}
        // #253: rulers, grid, guides and snap to them.
        case "canvas-view.rulers": return {enabled: has, checkable: true, checked: showRulers}
        case "canvas-view.grid": return {enabled: has, checkable: true, checked: showGrid}
        case "canvas-view.guides": return {enabled: has, checkable: true, checked: showGuides}
        case "canvas-view.snap-to": return {enabled: has, checkable: true, checked: snapToLines}
        case "canvas-view.clear-guides": return {enabled: guidesData.guides.length > 0}
        case "canvas-view.tool-hand": return tool("hand")
        case "canvas-view.tool-zoom": return tool("zoom")
        case "canvas-view.tool-move": return tool("move")
        case "canvas-view.free-transform": return {enabled: freeTransform || (canvasData.layersEditable && layersPanel.current !== null), checkable: true, checked: freeTransform} // a group says why in words when it is asked
        case "canvas-view.tool-region": return tool("region")
        case "canvas-view.tool-lasso": return tool("lasso")
        case "canvas-view.tool-brush": return {enabled: has, checkable: true, checked: root.tool === "brush"}
        case "canvas-view.tool-eraser": return tool("eraser")
        case "canvas-view.brush-swap": case "canvas-view.brush-smaller": case "canvas-view.brush-larger": return {enabled: brushing}
        }
        return undefined
    }
    // Menu `index` of the bar, as CommandMenu shows it: the registry's rows with this view's state put in.
    function menuItems(index) {
        const decorate = items => items.map(item => {
            if (item.items !== undefined)
                return Object.assign({}, item, {items: decorate(item.items)})
            const state = item.command ? menuState(item.command) : undefined
            return state === undefined ? item : Object.assign({}, item, state)
        })
        return decorate(App.commands.menu("canvas-view")[index].items)
    }
    // A command of the strip (the registry's `canvas-view.*`), from a key or a menu. While Compare is on the picture has no
    // region to draw and no mask overlay, so M steps through the compare modes and \ flips A and B, as in Compare Images
    // (`compare.next-mode`, `compare.flip`), and keep their Photoshop jobs otherwise.
    function runTool(id) {
        switch (id) {
        case "canvas-view.tool-hand": setTool("hand"); break
        case "canvas-view.tool-zoom": setTool("zoom"); break
        case "canvas-view.tool-move": setTool("move"); break
        case "canvas-view.free-transform": if (freeTransform) commitTransform(); else startFreeTransform(); break
        case "canvas-view.tool-lasso": setTool("lasso"); break
        case "canvas-view.tool-region": if (compareOn) pictureView.nextMode(); else setTool("region"); break
        case "canvas-view.tool-brush": setTool("brush"); break
        case "canvas-view.tool-eraser": setTool("eraser"); break
        case "canvas-view.brush-swap": if (brushing) tool = tool === "brush" ? "eraser" : "brush"; break
        case "canvas-view.brush-smaller": stepSize(false); break
        case "canvas-view.brush-larger": stepSize(true); break
        case "canvas-view.quick-mask": toggleQuickMask(); break
        case "canvas-view.add-mask": addMaskToCurrent(); break
        case "canvas-view.new-layer": canvasData.newLayer(); break
        case "canvas-view.merge-down": if (mergeable) canvasData.mergeDown(layersPanel.current.position); break
        case "canvas-view.flatten": canvasData.flatten(); break
        case "canvas-view.group": canvasData.groupLayers(layersPanel.chosen); break
        case "canvas-view.ungroup": canvasData.ungroup(layersPanel.chosen); break
        case "canvas-view.layer-properties": if (layersPanel.current) fileDialogs.askProperties(layersPanel.current.position); break
        case "canvas-view.mask-overlay": if (compareOn) pictureView.flip(); else maskOverlay = !maskOverlay; break
        case "canvas-view.mask-invert": if (quickMask) selectionLayer.invert(); else canvasData.invertMask(); break
        // Undo follows what is being done: a Quick Mask stroke, a layer mask's revision, else the document's (the region).
        case "canvas-view.new": newCanvas(); break
        case "canvas-view.open": chooseCanvas(); break
        case "canvas-view.import": importPicture(); break
        case "canvas-view.export": exportDialogOpen(); break
        case "canvas-view.export-options": exportWith("picture"); break
        case "canvas-view.export-layers": exportWith("layers"); break
        case "canvas-view.rename": fileDialogs.askRename(); break
        case "canvas-view.share": fileDialogs.askShare(); break
        case "canvas-view.batch": batchDialog.openDialog(); break
        case "canvas-view.duplicate": canvasData.duplicateCanvas(); break
        case "canvas-view.clear": fileDialogs.askClear(); break
        case "canvas-view.flip-horizontal": if (transformable) canvasData.flipLayer(layersPanel.current.position, true); break
        case "canvas-view.flip-vertical": if (transformable) canvasData.flipLayer(layersPanel.current.position, false); break
        case "canvas-view.turn-clockwise": if (transformable) canvasData.turnLayer(layersPanel.current.position, true); break
        case "canvas-view.turn-counterclockwise": if (transformable) canvasData.turnLayer(layersPanel.current.position, false); break
        case "canvas-view.paste-layer": pasteLayerOrNodes(); break
        case "canvas-view.paste-canvas": pasteAsNewCanvas(); break
        case "canvas-view.place-picture": placePicture(); break
        case "canvas-view.delete": fileDialogs.askDelete(); break
        case "canvas-view.close": closeCanvas(); break
        case "canvas-view.select-all": selectAll(); break
        case "canvas-view.deselect": deselect(); break
        case "canvas-view.invert-selection": if (quickMask || maskSelected) { selectionLayer.invert(); selectionLast = true } break
        case "canvas-view.use-selection": if (quickMask) useSelection(); break
        case "canvas-view.selection-expand": selectionDialogs.askModify("expand"); break
        case "canvas-view.selection-contract": selectionDialogs.askModify("contract"); break
        case "canvas-view.selection-feather": selectionDialogs.askModify("feather"); break
        case "canvas-view.selection-border": selectionDialogs.askModify("border"); break
        case "canvas-view.selection-smooth": selectionDialogs.askModify("smooth"); break
        case "canvas-view.selection-transform": selectionDialogs.askTransform(); break
        case "canvas-view.mask-reveal": canvasData.fillMask(); break
        case "canvas-view.mask-hide": canvasData.clearMask(); break
        case "canvas-view.fit": fit(); break
        case "canvas-view.actual-size": actualSize(); break
        case "canvas-view.zoom-in": pictureView.zoomAt(pictureView.width / 2, pictureView.height / 2, 1.25); break
        case "canvas-view.zoom-out": pictureView.zoomAt(pictureView.width / 2, pictureView.height / 2, 0.8); break
        case "canvas-view.history": showHistory = !showHistory; break
        case "canvas-view.layers": showLayers = !showLayers; break
        case "canvas-view.compare": comparing = !comparing; break
        case "canvas-view.details": advancedDetails = !advancedDetails; break
        case "canvas-view.snap": snap = !snap; break
        case "canvas-view.rulers": showRulers = !showRulers; break
        case "canvas-view.grid": showGrid = !showGrid; break
        case "canvas-view.guides": showGuides = !showGuides; break
        case "canvas-view.snap-to": snapToLines = !snapToLines; break
        case "canvas-view.clear-guides": guidesData.clearGuides(); break
        case "canvas-view.undo":
            if (selectionUndoes) selectionLayer.undo()
            else if (paintsMask || canvasData.maskPending) canvasData.undo()
            else App.commands.trigger("edit.undo")
            break
        case "canvas-view.redo":
            if (selectionUndoes) selectionLayer.redo()
            else if (paintsMask) canvasData.redo()
            else App.commands.trigger("edit.redo")
            break
        }
    }
    HoverHandler { id: viewHover }
    Connections {
        target: App.commands
        function onWindowRequested(id) {
            if (!root.keysHot || !id.startsWith("canvas-view."))
                return
            const state = root.menuState(id)
            if (state === undefined || state.enabled)
                root.runTool(id)
            else if (id === "canvas-view.paste-layer") // not on the latest version: the key still belongs to the Graph's paste
                App.commands.trigger("edit.paste")
        }
    }
    // Escape clears the selection wherever the focus is, as Ctrl+D does (a key to a focused item would miss it after a click
    // elsewhere in the dialog); `escapeDeselects` is false while a drag or a run wants the key.
    Shortcut {
        sequence: "Escape"
        enabled: root.keysHot && root.lassoDragging
        onActivated: brushPen.cancel()
    }
    Shortcut {
        sequence: "Escape"
        enabled: root.keysHot && root.escapeDeselects && !root.lassoDragging
        onActivated: root.deselect()
    }
    // In a modal popup (the dialog) the window's shortcuts sleep: the view carries the keys of its own commands.
    Instantiator {
        model: root.ownShortcuts ? App.commands.activeKeys.filter(k => App.commands.ownerOf(k).startsWith("canvas-view.")) : []
        delegate: Shortcut {
            required property string modelData
            sequence: modelData
            enabled: root.keysHot && !(App.commands.textFieldFocus && App.commands.belongsToTextField(modelData))
            onActivated: App.commands.triggerKey(modelData)
        }
    }

    // The canvas's guides and grid spacing (#253): a sidecar file beside the canvas, not a revision, and the snap function.
    GuidesModel {
        id: guidesData
        objectName: "canvasViewGuides"
        storeRoot: canvasData.storeRoot
        canvasId: canvasData.canvasId
        canvasSize: canvasData.pictureSize
    }
    CanvasViewModel {
        id: canvasData
        onCanvasChanged: { root.freeRect = root.noRect; root.selectionLast = false; selectionLayer.reset() }
        onPictureChanged: root.selectionLast = false
        maskLayer: layerMask
        onFollowChanged: root.freeRect = root.noRect
    }
    // Delayed: following a node moves the view to its canvas, which changes the nodes that read the canvas shown.
    Binding {
        target: canvasData
        property: "followNode"
        value: root.followTarget
        delayed: true
    }
    readonly property bool dragging: (regionFrame.item as CropFrame)?.dragging ?? false
    readonly property bool lassoDragging: brushPen.lassoing
    // The Active canvas (#246): the canvas this view shows, once it is used (a press on the picture, the canvas changed, the
    // view opened). A window with no such object (a test's) has none.
    function reportActive() {
        if (visible && canvasData.canvasId !== "" && App.activeCanvas !== null)
            App.activeCanvas.use(canvasData.canvasId)
    }
    onVisibleChanged: reportActive()
    Connections {
        target: canvasData
        function onCanvasChanged() { root.reportActive() }
    }
    // An action that makes a new canvas (Upscale) finished: the view that was showing the source shows the new one, which is its own
    // canvas; the source is as it was and is in the list (File > Open canvas…).
    Connections {
        target: App.canvasActions
        enabled: App.canvasActions !== null
        function onCanvasMade(source, made) {
            if (source !== canvasData.canvasId)
                return
            canvasData.useCanvas(made)
            canvasData.showNotice(qsTr("Made a new canvas: the original is as it was (File > Open canvas…)."))
        }
    }
    Component.onCompleted: {
        // One canvas by default: with none chosen, the newest.
        if (canvasData.canvasId === "" && canvasData.canvases.length > 0)
            canvasData.canvasId = canvasData.canvases[0].id
        reportActive()
    }
    onCompareOnChanged: pictureView.setView({mode: compareOn ? modeButtons.remembered() : "single"})

    // The picture is saved by the file dialog, as Save Image does: the model writes it and never replaces a file.
    FilePicker {
        id: exportDialog
        objectName: "canvasViewExportDialog"
        title: qsTr("Save the revision as a picture")
        fileMode: PortalFileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: [qsTr("PNG images (*.png)"), qsTr("JPEG images (*.jpg *.jpeg)"), qsTr("WebP images (*.webp)"), qsTr("Layered image (*.ora)"), qsTr("Photoshop file (*.psd)")]
        onAccepted: canvasData.exportPicture(selectedFile.toString())
    }
    // Batch… (#250): a canvas action over a folder of pictures.
    CanvasBatchDialog {
        id: batchDialog
        parent: Overlay.overlay
    }
    CanvasFileDialogs {
        id: fileDialogs
        view: root
    }
    CanvasSelectionDialogs {
        id: selectionDialogs
        onModifyAsked: (kind, amount) => root.modifySelection(kind, amount)
        onTransformAsked: (dx, dy, scaleX, scaleY, degrees) => root.transformSelection(dx, dy, scaleX, scaleY, degrees)
    }
    Component.onDestruction: {
        exportDialog.close()
        if (exportDialogBox)
            exportDialogBox.close()
        if (keysHot)
            App.commands.setContextActive("canvas-view", false)
    }

    // New canvas… and Import picture… (#228, C7), as the canvas picker has them: the canvas is made in this store and shown.
    // The New canvas dialog (#232), made the first time: the canvas is made in this view's store and shown at once.
    property var newCanvasDialog: null
    function newCanvas() {
        if (!newCanvasDialog)
            newCanvasDialog = newCanvasComponent.createObject(root, { storeRoot: canvasData.storeRoot })
        newCanvasDialog.openNew("")
    }
    function importPicture() {
        (pictureDialog.createObject(root) as FilePicker).open()
    }
    // Paste as new layer, Place picture… and a dropped file (#236): the picture is put on the canvas as a layer, centered in
    // what the view shows of the canvas (a drop: where it fell). Each is one revision; a problem is said under the picture.
    function visibleCenter() {
        const k = pictureView.imageScale, size = canvasData.pictureSize
        if (k <= 0 || size.width <= 0)
            return {x: -1, y: -1}
        const span = (offset, shown, whole) => {
            const from = Math.max(0, -offset / k), to = Math.min(whole, (shown - offset) / k)
            return to > from ? (from + to) / 2 : whole / 2
        }
        return {x: span(pictureView.imageX, pictureView.width, size.width), y: span(pictureView.imageY, pictureView.height, size.height)}
    }
    // Ctrl+V over this view: a picture on the clipboard is a layer; anything else is the Graph's paste (nodes), which this view's key hides.
    function pasteLayerOrNodes() {
        if (!canvasData.clipboardHasPicture() && App.commands.trigger("edit.paste"))
            return
        pasteLayer()
    }
    function pasteLayer() {
        const at = visibleCenter()
        canvasData.pasteLayer(at.x, at.y)
    }
    function placeFiles(urls, x, y) {
        for (const url of urls)
            if (!canvasData.placeFile(url.toString(), x, y))
                break // the problem is said under the picture; the rest would only repeat it
    }
    function placePicture() {
        (placeDialog.createObject(root) as FilePicker).open()
    }
    function pasteAsNewCanvas() {
        if (!newCanvasDialog)
            newCanvasDialog = newCanvasComponent.createObject(root, { storeRoot: canvasData.storeRoot })
        const why = newCanvasDialog.model.pasteAsNew()
        if (why !== "")
            newCanvasDialog.openNew(why)
    }
    Component {
        id: exportComponent
        CanvasExportDialog {
            parent: Overlay.overlay
            onSaved: notice => canvasData.showNotice(notice)
        }
    }
    Component {
        id: newCanvasComponent
        NewCanvasDialog {
            id: madeDialog
            parent: Overlay.overlay
            onCreated: id => {
                root.makeProblem = ""
                canvasData.useCanvas(id)
                if (madeDialog.model.importNotes() !== "") // a Photoshop file's layers Fugue could not keep: never silent
                    canvasData.showNotice(qsTr("Imported, with changes: %1").arg(madeDialog.model.importNotes()))
            }
        }
    }
    Component {
        id: placeDialog
        FilePicker {
            id: d
            objectName: "canvasViewPlaceDialog"
            title: qsTr("Place a picture on the canvas")
            fileMode: PortalFileDialog.OpenFile
            nameFilters: [qsTr("Pictures (*.png *.jpg *.jpeg *.webp)"), qsTr("All files (*)")]
            onAccepted: { const at = root.visibleCenter(); canvasData.placeFile(d.selectedFile.toString(), at.x, at.y); d.destroy() }
            onRejected: d.destroy()
        }
    }
    Component {
        id: pictureDialog
        FilePicker {
            id: d
            objectName: "canvasViewImportDialog"
            title: qsTr("Import a picture as a canvas")
            fileMode: PortalFileDialog.OpenFile
            nameFilters: [qsTr("Pictures and canvases (*.png *.jpg *.jpeg *.webp *.ora *.psd *.fuguepack)"), qsTr("All files (*)")]
            onAccepted: { root.makeProblem = canvasData.importPicture(d.selectedFile, ""); d.destroy() }
            onRejected: d.destroy()
        }
    }

    // A list to choose from, under the button that asked: the canvases, the earlier revisions.
    Component {
        id: menuComponent
        WidgetMenu {
            id: menu
            look: root.look
            parent: Overlay.overlay
            onClosed: menu.destroy()
        }
    }
    function openMenu(button, entries, picked, wide) {
        const at = button.mapToItem(Overlay.overlay, 0, button.height)
        const menu = menuComponent.createObject(root, {wantX: at.x, wantY: at.y, width: Math.max(button.width, (wide ?? 40) * u),
                                                       anchorHeight: button.height, entries: entries, searchable: entries.length > 12}) as WidgetMenu
        menu.picked.connect(picked)
        menu.open()
    }
    // The canvas picker's own rows (canvasMenu.js): name, size and last change, the id, then New canvas… and Import picture….
    function chooseCanvas() {
        openMenu(canvasButton, CanvasMenu.entries(canvasData.canvases, canvasData.canvasId, ""), value => {
            if (value.action === "new")
                root.newCanvas()
            else if (value.action === "import")
                root.importPicture()
            else if (value.id !== undefined)
                canvasData.canvasId = value.id
        }, 80) // the rows carry a name, a size and a time, and the id under them
    }
    // A rectangle drawn with no node followed: kept, and with several Region nodes on this canvas, which one is it for?
    function placeFree(r) {
        freeRect = r
        if (regionsHere.length > 1)
            chooseRegion()
    }
    function chooseRegion() {
        const frame = regionFrame.item as CropFrame
        const anchor = frame ?? regionLayer
        const x = frame ? frame.fx : 0, y = frame ? frame.fy + frame.fh : 0
        const at = anchor.mapToItem(Overlay.overlay, x, y)
        const menu = menuComponent.createObject(root, {wantX: at.x, wantY: at.y + u, width: 56 * u, anchorHeight: 0,
                                                       entries: [{heading: true, enabled: false, text: qsTr("Which Canvas Region is it for?")}]
                                                                .concat(regionsHere.map(node => ({text: node.name, value: node.id})))}) as WidgetMenu
        menu.objectName = "canvasViewRegionChooser"
        menu.picked.connect(id => root.assignFree(id))
        menu.open()
    }
    // The drawn rectangle becomes the node's (one undo step) and the node is the target from now on.
    function assignFree(node) {
        const r = freeRect
        chosenRegion = node
        canvasData.followNode = node // at once, not when the delayed binding catches up
        if (root.regionEditable)
            canvasData.setRegion(r)
    }
    function addRegionNode() {
        const node = canvasData.edits ? canvasData.edits.addRegionNode(canvasData.canvasId, freeRect) : ""
        if (node !== "")
            chosenRegion = node
    }
    // The pro's numbers (Advanced details): the context and the model's size, what is shown, what changed in it.
    readonly property string detailsText: {
        const parts = []
        if (previewReady) {
            parts.push(qsTr("Context %1 × %2").arg(preview.context.width).arg(preview.context.height))
            parts.push(qsTr("Model %1 × %2%3").arg(preview.modelWidth).arg(preview.modelHeight)
                       .arg(preview.capped ? qsTr(" (at the %1 MP cap)").arg(canvasData.region.cap) : ""))
        }
        if (regionValid)
            parts.push(qsTr("Reads %1").arg(readsText(canvasData.region)))
        parts.push(qsTr("Showing %1").arg(labelOf(rowB)))
        return parts.concat(canvasData.summary).join(" · ")
    }
    function readsText(region) {
        return region.reads === "head" || region.reads === "" ? qsTr("the latest version")
             : region.readsId !== "" ? qsTr("“%1”").arg(canvasData.stepName(region.readsId)) : qsTr("an unknown step")
    }

    Keys.onReleased: event => {
        if (event.key === Qt.Key_Space && !event.isAutoRepeat)
            panKey = false
    }
    // Before the window's shortcuts (in an area they are awake): Escape while a drag is on is the drag's (not Cancel the
    // run's), a bare Q is not the window's quit here, and a key of the Graph (Home, ., Delete, Ctrl+A, Tab…) does not
    // reach the Graph behind this view; its own keys below take it, or nothing does (see the header). Escape otherwise
    // stays the window's and the dialog's (it closes).
    Keys.onShortcutOverride: event => {
        const bare = !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
        if (event.key === Qt.Key_Escape) {
            event.accepted = root.freeTransform || root.dragging || taskBar.busy || root.escapeDeselects // the box's, a drag's, the task bar's Cancel, or Deselect
            return
        }
        if (root.freeTransform && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !App.commands.textFieldFocus) {
            event.accepted = true // Enter keeps the transform, not the host dialog's confirm
            return
        }
        const owner = App.commands.ownerOfPress(event.key, event.modifiers)
        // The bare Q is Quick Mask's here (#230, the `canvas-view` context); it never quits from the view.
        event.accepted = (event.key === Qt.Key_Q && bare && !owner.startsWith("canvas-view.")) || (owner !== "" && App.commands.actsOnGraph(owner))
    }
    Keys.onPressed: event => {
        if (root.freeTransform && event.key === Qt.Key_Escape) {
            root.endTransformSession()
            event.accepted = true
            return
        }
        if (root.freeTransform && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            root.commitTransform()
            event.accepted = true
            return
        }
        if ((root.freeTransform || (root.tool === "move" && root.transformable)) && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
            const step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
            const arrows = {[Qt.Key_Left]: [-1, 0], [Qt.Key_Right]: [1, 0], [Qt.Key_Up]: [0, -1], [Qt.Key_Down]: [0, 1]}
            if (arrows[event.key] !== undefined) {
                root.nudgeLayer(arrows[event.key][0] * step, arrows[event.key][1] * step)
                event.accepted = true
                return
            }
        }
        if (event.key === Qt.Key_Escape) {
            const frame = regionFrame.item as CropFrame
            if ((!frame || !frame.cancel()) && !taskBar.cancelIfBusy()) {
                if (!root.escapeDeselects)
                    return // not ours: the dialog closes
                root.deselect()
            }
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Space) {
            panKey = true
            event.accepted = true
            return
        }
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
            return
        if (event.key === Qt.Key_Q) { event.accepted = true; return }
        if (event.key === Qt.Key_Home || event.key === Qt.Key_0) fit()
        else if (event.key === Qt.Key_1) actualSize()
        else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) pictureView.zoomAt(pictureView.width / 2, pictureView.height / 2, 1.25)
        else if (event.key === Qt.Key_Minus) pictureView.zoomAt(pictureView.width / 2, pictureView.height / 2, 0.8)
        else return
        event.accepted = true
    }

    // A chunk of the tools: its buttons 1u apart, 3u more before it than inside it (4u from the chunk before, with the
    // Flow's 1u: twice the gap inside, docs/ui-ergonomics.md Gaps); the first chunk shown has no lead.
    component Chunk: Row {
        property bool lead: true
        spacing: root.u
        leftPadding: lead && !root.mirrored ? 3 * root.u : 0
        rightPadding: lead && root.mirrored ? 3 * root.u : 0
    }

    // One bar (#228, the owner's decision: no strip of revisions, no hashes): the canvas, the History and Layers panels,
    // then the tools. It wraps when the view is narrow.
    CanvasMenuBar {
        id: menuBar
        look: root.look
        view: root
        width: parent.width
        height: implicitHeight
    }
    Flow {
        id: tools
        objectName: "canvasViewTools"
        anchors.top: menuBar.bottom
        anchors.topMargin: root.u
        width: parent.width
        spacing: root.u
        Chunk {
            lead: false
            ViewButton {
                id: canvasButton
                objectName: "canvasViewCanvas"
                look: root.look
                text: canvasData.canvases.find(row => row.id === canvasData.canvasId)?.name ?? qsTr("Canvas")
                trailingGlyph: "chevron-down"
                tip: qsTr("Choose the canvas to look at, or make one")
                accented: false
                onTapped: root.chooseCanvas()
            }
        }
        // How the shown revision is looked at: alone, or against another (the modes).
        Chunk {
            visible: canvasData.canvasId !== ""
            CompareModeButtons {
                id: modeButtons
                visible: root.compareOn
                look: root.look
                prefix: "canvasViewMode-"
                accented: false
                mode: pictureView.mode
                hasDifference: canvasData.sourceDifference.toString() !== ""
                onPicked: picked => pictureView.setView({mode: picked})
            }
        }
        // Fit and 100% at the end, then the host's: as every large view's bar.
        Chunk {
            visible: root.zoomButtons && pictureView.visible
            ViewButton {
                objectName: "canvasViewFit"
                look: root.look
                text: qsTr("Fit")
                tip: qsTr("Fit (Home)")
                accented: false
                current: !pictureView.actual && pictureView.zoom <= 1
                onTapped: root.fit()
            }
            ViewButton {
                objectName: "canvasViewActual"
                look: root.look
                text: qsTr("100%")
                tip: qsTr("Actual size (1)")
                accented: false
                current: pictureView.actual
                onTapped: root.actualSize()
            }
        }
        Chunk {
            id: trailingChunk
            lead: canvasData.canvasId !== "" // with no canvas it is the row's only chunk
            visible: children.length > 0
        }
    }


    // The options of the current tool (#230), one row over the picture (Photoshop's options bar, Blender's tool
    // settings): never a text block, only what the tool needs to act. It wraps in a narrow area.
    Flow {
        id: optionsBar
        objectName: "canvasViewOptions"
        anchors.top: tools.bottom
        anchors.topMargin: visible ? root.u : 0
        width: parent.width
        height: visible ? implicitHeight : 0
        visible: canvasData.canvasId !== ""
        spacing: root.u
        Text {
            objectName: "canvasViewToolName"
            height: 7 * root.u
            verticalAlignment: Text.AlignVCenter
            rightPadding: root.u
            color: root.look.label
            font.pixelSize: root.look.labelSize
            text: root.freeTransform ? qsTr("Free transform") : root.quickMask && root.brushing ? qsTr("Quick Mask") : root.tool === "hand" ? qsTr("Hand") : root.tool === "zoom" ? qsTr("Zoom")
                  : root.tool === "brush" ? qsTr("Mask brush") : root.tool === "eraser" ? qsTr("Mask eraser") : root.tool === "move" ? qsTr("Move") : qsTr("Rectangle region")
        }
        // The region: Snap to the node's Multiple of.
        ViewButton {
            objectName: "canvasViewSnap"
            visible: root.tool === "region" && root.regionValid
            look: root.look
            enabled: root.regionEditable && canvasData.region.multiple > 1
            text: qsTr("Snap to %1").arg(canvasData.region.multiple)
            tip: qsTr("Put the region's edges on multiples of the node's Multiple of; with a ratio held only the corner moves")
            accented: false
            checkable: true
            current: root.snap
            onTapped: root.snap = !root.snap
        }
        // The grid's spacing (#253), while the grid is on: the canvas's own (kept beside it).
        DragNumberField {
            objectName: "canvasViewGridSpacing"
            visible: root.showGrid
            width: 30 * root.u
            look: root.look
            label: qsTr("Grid")
            tip: qsTr("The grid's spacing in canvas pixels (%1)").arg(root.keysOf("canvas-view.grid"))
            value: guidesData.gridSpacing
            from: guidesData.minGrid
            to: 512
            step: 1
            snap: 8
            defaultValue: guidesData.defaultGrid
            format: v => qsTr("%1 px").arg(Math.round(v))
            parse: text => Number(String(text).replace("px", "").trim())
            onCommitted: v => guidesData.setGridSpacing(Math.round(v))
        }
        // Free transform (#236): where the layer is, how big, how turned, how it is resampled; Apply keeps it. Made only while a box is open: its
        // fields would otherwise be one more dozen objects in every Canvas view that never transforms anything.
        Loader {
            id: transformOptions
            active: root.freeTransform && root.xfPlacement !== null
            visible: active
            sourceComponent: Flow {
                width: optionsBar.width // wraps as the options bar does, not one overflowing row
                spacing: root.u
        // Free transform (#236): where the layer is (its center), how big, how turned, and how it is resampled; Apply keeps it.
        Chunk {
            lead: false
            DragNumberField {
                objectName: "canvasViewTransformX"
                width: 30 * root.u
                look: root.look
                label: qsTr("X")
                tip: qsTr("Where the layer's center is, across, in canvas pixels")
                value: root.xfPlacement ? root.xfPlacement.cx : 0
                from: -100000
                to: 100000
                step: 1
                snap: 1
                defaultValue: 0
                format: v => qsTr("%1 px").arg(Math.round(v * 10) / 10)
                parse: text => Number(String(text).replace("px", "").trim())
                onCommitted: v => root.xfPlacement = xfGesture.changed(root.xfPlacement, {cx: v}, false)
            }
            DragNumberField {
                objectName: "canvasViewTransformY"
                width: 30 * root.u
                look: root.look
                label: qsTr("Y")
                tip: qsTr("Where the layer's center is, down, in canvas pixels")
                value: root.xfPlacement ? root.xfPlacement.cy : 0
                from: -100000
                to: 100000
                step: 1
                snap: 1
                defaultValue: 0
                format: v => qsTr("%1 px").arg(Math.round(v * 10) / 10)
                parse: text => Number(String(text).replace("px", "").trim())
                onCommitted: v => root.xfPlacement = xfGesture.changed(root.xfPlacement, {cy: v}, false)
            }
            DragNumberField {
                objectName: "canvasViewTransformW"
                width: 30 * root.u
                look: root.look
                label: qsTr("W")
                tip: qsTr("The layer's width in canvas pixels, before it is turned")
                value: root.xfPlacement ? root.xfPlacement.width : 1
                from: 1
                to: 32768
                step: 1
                snap: 10
                defaultValue: root.xfStart ? root.xfStart.width : 1
                format: v => qsTr("%1 px").arg(Math.round(v * 10) / 10)
                parse: text => Number(String(text).replace("px", "").trim())
                onCommitted: v => root.xfPlacement = xfGesture.changed(root.xfPlacement, {width: v}, root.xfLink)
            }
            ViewButton {
                objectName: "canvasViewTransformLink"
                look: root.look
                text: qsTr("Link")
                accented: false
                checkable: true
                current: root.xfLink
                tip: qsTr("Keep the proportions when the width or the height is typed")
                onTapped: root.xfLink = !root.xfLink
            }
            DragNumberField {
                objectName: "canvasViewTransformH"
                width: 30 * root.u
                look: root.look
                label: qsTr("H")
                tip: qsTr("The layer's height in canvas pixels, before it is turned")
                value: root.xfPlacement ? root.xfPlacement.height : 1
                from: 1
                to: 32768
                step: 1
                snap: 10
                defaultValue: root.xfStart ? root.xfStart.height : 1
                format: v => qsTr("%1 px").arg(Math.round(v * 10) / 10)
                parse: text => Number(String(text).replace("px", "").trim())
                onCommitted: v => root.xfPlacement = xfGesture.changed(root.xfPlacement, {height: v}, root.xfLink)
            }
            DragNumberField {
                objectName: "canvasViewTransformAngle"
                width: 30 * root.u
                look: root.look
                label: qsTr("Angle")
                tip: qsTr("How far the layer is turned, in degrees, clockwise")
                value: root.xfPlacement ? root.xfPlacement.angle : 0
                from: -180
                to: 180
                step: 1
                snap: 15
                defaultValue: 0
                format: v => qsTr("%1°").arg(Math.round(v * 10) / 10)
                parse: text => Number(String(text).replace("°", "").trim())
                onCommitted: v => root.xfPlacement = xfGesture.changed(root.xfPlacement, {angle: v}, false)
            }
        }
        Chunk {
            lead: false
            Repeater {
                model: [{id: "lanczos", text: qsTr("Lanczos"), tip: qsTr("The sharpest resampling for photographs (the default)")},
                        {id: "bicubic", text: qsTr("Bicubic"), tip: qsTr("A smoother resampling")},
                        {id: "nearest", text: qsTr("Nearest"), tip: qsTr("No blending: hard pixels, for pixel art")}]
                delegate: ViewButton {
                    required property var modelData
                    objectName: "canvasViewTransformFilter-" + modelData.id
                    look: root.look
                    text: modelData.text
                    tip: modelData.tip
                    accented: false
                    checkable: true
                    current: root.xfPlacement !== null && root.xfPlacement.filter === modelData.id
                    onTapped: root.xfPlacement = xfGesture.changed(root.xfPlacement, {filter: modelData.id}, false)
                }
            }
        }
        Chunk {
            lead: false
            ViewButton {
                objectName: "canvasViewTransformApply"
                look: root.look
                text: qsTr("Apply")
                tip: qsTr("Keep the transform as one step (%1)").arg(qsTr("Enter"))
                onTapped: root.commitTransform()
            }
            ViewButton {
                objectName: "canvasViewTransformCancel"
                look: root.look
                text: qsTr("Cancel")
                tip: qsTr("Leave the layer as it was (%1)").arg(qsTr("Escape"))
                accented: false
                onTapped: root.endTransformSession()
            }
        }
            }
        }
        Text {
            objectName: "canvasViewMoveHint"
            visible: root.tool === "move" && !root.freeTransform
            height: 7 * root.u
            verticalAlignment: Text.AlignVCenter
            color: root.look.label
            font.pixelSize: root.look.captionSize
            text: qsTr("Drag to move the current layer; arrow keys nudge it (Shift: 10 px); %1 transforms it").arg(root.keysOf("canvas-view.free-transform"))
        }
        // Zoom: in or out with a click (Alt does the other).
        Chunk {
            lead: false
            visible: root.tool === "zoom"
            ViewButton {
                objectName: "canvasViewZoomIn"
                look: root.look
                text: qsTr("Zoom in")
                tip: qsTr("A click zooms in; Alt+click zooms out")
                accented: false
                checkable: true
                current: !root.zoomOut
                onTapped: root.zoomOut = false
            }
            ViewButton {
                objectName: "canvasViewZoomOut"
                look: root.look
                text: qsTr("Zoom out")
                tip: qsTr("A click zooms out; Alt+click zooms in")
                accented: false
                checkable: true
                current: root.zoomOut
                onTapped: root.zoomOut = true
            }
        }
        // The brush: what it paints, then its settings, then the mask's own.
        Rectangle {
            objectName: "canvasViewBrushTarget"
            visible: root.brushing && root.paintTarget !== null
            width: targetRow.implicitWidth + 4 * root.u
            height: 7 * root.u
            radius: root.look.smallCorner
            color: root.look.selectedRow
            Accessible.role: Accessible.StaticText
            Accessible.name: targetText.text
            Row {
                id: targetRow
                anchors.centerIn: parent
                spacing: root.u
                Glyph {
                    anchors.verticalCenter: parent.verticalCenter
                    look: root.look
                    name: root.quickMask ? "circle-dashed" : "layers"
                    color: root.look.text
                }
                Text {
                    id: targetText
                    objectName: "canvasViewBrushTargetText"
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.look.text
                    font.pixelSize: root.look.labelSize
                    text: root.quickMask ? qsTr("Selection") : qsTr("Mask of %1").arg(root.maskRow ? root.maskRow.name : "")
                }
            }
        }
        Chunk {
            lead: false
            visible: root.brushing && root.paintTarget === null
            ViewButton {
                objectName: "canvasViewBrushAddMask"
                look: root.look
                enabled: canvasData.layersEditable && root.layersPanel.current !== null && root.layersPanel.current.kind !== "mask"
                text: qsTr("Add mask")
                tip: root.layersPanel.current ? qsTr("Give %1 a layer mask and paint it (%2)").arg(root.layersPanel.current.name).arg(root.keysOf("canvas-view.add-mask"))
                                         : qsTr("Select a layer to give it a mask")
                onTapped: root.addMaskToCurrent()
            }
            ViewButton {
                objectName: "canvasViewBrushQuickMask"
                look: root.look
                glyph: "circle-dashed"
                text: qsTr("Quick Mask")
                tip: qsTr("Paint the part to change as a selection (%1)").arg(root.keysOf("canvas-view.quick-mask"))
                onTapped: root.toggleQuickMask()
            }
        }
        Chunk {
            visible: root.brushing
            DragNumberField {
                objectName: "canvasViewBrushSize"
                width: 30 * root.u
                look: root.look
                label: qsTr("Size")
                tip: qsTr("The brush's size in canvas pixels ([ and ], Alt+drag on the picture)")
                value: root.brushSize
                from: 1
                to: root.maxBrush
                step: 1
                snap: 16
                defaultValue: 64
                format: v => qsTr("%1 px").arg(Math.round(v))
                parse: text => Number(String(text).replace("px", "").trim())
                onCommitted: v => root.setSize(v)
            }
            DragNumberField {
                objectName: "canvasViewBrushHardness"
                width: 30 * root.u
                look: root.look
                label: qsTr("Hardness")
                tip: qsTr("How sharp the brush's edge is (Alt+drag up and down on the picture)")
                value: root.brushHardness
                from: 0
                to: 1
                step: 0.01
                snap: 0.1
                defaultValue: 0.8
                format: v => qsTr("%1%").arg(Math.round(v * 100))
                parse: text => Number(String(text).replace("%", "").trim()) / 100
                onCommitted: v => root.setHardness(v)
            }
            DragNumberField {
                objectName: "canvasViewBrushOpacity"
                width: 30 * root.u
                look: root.look
                label: qsTr("Opacity")
                tip: qsTr("How much one stroke covers")
                value: root.brushOpacity
                from: 0.01
                to: 1
                step: 0.01
                snap: 0.1
                defaultValue: 1
                format: v => qsTr("%1%").arg(Math.round(v * 100))
                parse: text => Number(String(text).replace("%", "").trim()) / 100
                onCommitted: v => root.brushOpacity = v
            }
        }
        Chunk {
            visible: root.brushing && root.paintTarget !== null
            ViewButton {
                objectName: "canvasViewMaskOverlay"
                visible: root.paintsMask
                look: root.look
                text: qsTr("Overlay")
                tip: qsTr("Show the hidden part of the mask in red (%1)").arg(root.keysOf("canvas-view.mask-overlay"))
                accented: false
                checkable: true
                current: root.maskOverlay
                onTapped: root.maskOverlay = !root.maskOverlay
            }
            ViewButton {
                id: maskMenuButton
                objectName: "canvasViewMaskMenu"
                look: root.look
                glyph: "ellipsis"
                accented: false
                tip: root.quickMask ? qsTr("Invert or select all") : qsTr("Invert, reveal or hide the whole mask")
                onTapped: root.maskMenu(maskMenuButton)
            }
        }
        // Quick Mask: hand the selection to the run, or clear it (apart, last).
        Chunk {
            visible: root.quickMask
            ViewButton {
                objectName: "canvasViewUseSelection"
                look: root.look
                enabled: root.regionValid && canvasData.region.selectionWritable === true
                text: qsTr("Use selection")
                tip: !root.regionValid ? qsTr("Add or select a Canvas Region node first: it is what a run redraws")
                     : canvasData.region.selectionWritable !== true ? qsTr("This Canvas Region takes no selection")
                     : qsTr("Make %1 redraw the painted part (one undo step)").arg(root.followName)
                onTapped: root.useSelection()
            }
        }
        Text {
            objectName: "canvasViewSelectionProblem"
            visible: root.quickMask && root.selectionProblem !== ""
            height: 7 * root.u
            verticalAlignment: Text.AlignVCenter
            color: root.look.error
            font.pixelSize: root.look.labelSize
            text: root.selectionProblem
        }
    }

    // The tools (#230), a narrow column at the start of the picture; it scrolls in a short area, never wraps.
    Flickable {
        id: toolStrip
        objectName: "canvasViewToolStrip"
        anchors.top: pictureView.top
        anchors.bottom: pictureView.bottom
        anchors.left: parent.left
        width: 7 * root.u
        visible: canvasData.canvasId !== ""
        contentHeight: stripColumn.height
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        // A tool of the strip: its glyph, its name and key in the tooltip, the current one in the quiet marking (D7).
        component ToolButton: ViewButton {
            required property string toolName
            required property string command
            required property string name
            objectName: "canvasViewTool-" + toolName
            look: root.look
            accented: false
            checkable: true
            current: root.tool === toolName
            tip: qsTr("%1 (%2)").arg(name).arg(root.keysOf(command))
            onTapped: root.setTool(toolName)
        }
        Column {
            id: stripColumn
            spacing: root.u
            ToolButton { toolName: "hand"; command: "canvas-view.tool-hand"; glyph: "hand"; name: qsTr("Hand: drag to pan; Space held does it with any tool") }
            ToolButton { toolName: "zoom"; command: "canvas-view.tool-zoom"; glyph: "zoom-in"; name: qsTr("Zoom: click to zoom in, Alt+click to zoom out") }
            Item { width: 1; height: 2 * root.u }
            ToolButton { toolName: "move"; command: "canvas-view.tool-move"; glyph: "maximize-2"; name: qsTr("Move: drag the current layer; the arrow keys nudge it") }
            Item { width: 1; height: 2 * root.u }
            ToolButton { toolName: "region"; command: "canvas-view.tool-region"; glyph: "square-dashed"; name: qsTr("Rectangle region: drag the area a run redraws") }
            Item { width: 1; height: 2 * root.u }
            ToolButton { toolName: "lasso"; command: "canvas-view.tool-lasso"; glyph: "lasso-select"; name: qsTr("Lasso: draw the area freehand; Shift adds, Alt subtracts") }
            ToolButton { toolName: "brush"; command: "canvas-view.tool-brush"; glyph: "brush"; name: qsTr("Mask brush: paint a layer mask or the Quick Mask") }
            ToolButton { toolName: "eraser"; command: "canvas-view.tool-eraser"; glyph: "eraser"; name: qsTr("Mask eraser (X swaps with the brush)") }
            Item { width: 1; height: 2 * root.u }
            ViewButton {
                objectName: "canvasViewQuickMask"
                look: root.look
                glyph: "circle-dashed"
                accented: false
                checkable: true
                current: root.quickMask
                tip: qsTr("Quick Mask: paint the part to change, in red (%1)").arg(root.keysOf("canvas-view.quick-mask"))
                onTapped: root.toggleQuickMask()
            }
        }
    }

    // The picture, and over it the region of the followed node. Never mirrored: a picture is not text.
    CompareView {
        id: pictureView
        objectName: "canvasViewViewer"
        look: root.look
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        readonly property real startReserve: (toolStrip.visible ? toolStrip.width + 2 * root.u : 0) + (historyPanel.visible ? historyPanel.width + 2 * root.u : 0)
        readonly property real endReserve: layersPanel.visible ? layersPanel.width + 2 * root.u : 0
        x: root.mirrored ? endReserve : startReserve
        anchors.top: optionsBar.bottom
        anchors.topMargin: root.u
        anchors.bottom: notes.top
        anchors.bottomMargin: root.u
        width: parent.width - startReserve - endReserve
        visible: canvasData.canvasId !== "" && canvasData.revisionB !== ""
        onActiveFocusChanged: {
            if (!activeFocus)
                root.panKey = false
            else
                root.reportActive()
        }
        mode: "single"
        showZoom: true
        showSize: false // the status line under the picture says the size and the zoom
        // Alone, the picture is the revision shown; compared, A and B.
        // Each named as CanvasHistory names it.
        sourceA: root.compareOn ? canvasData.sourceA : canvasData.pictureSource
        sourceB: canvasData.sourceB
        sourceDifference: canvasData.sourceDifference
        sizeA: root.compareOn ? canvasData.sizeA : canvasData.pictureSize
        sizeB: canvasData.sizeB
        labelA: root.compareOn ? root.labelOf(root.rowA) : root.labelOf(root.rowB)
        labelB: root.labelOf(root.rowB)
    }
    // A picture file dropped on the picture is placed as a new layer where it fell (#236); anything else is left to the window's own drop.
    DropArea {
        id: pictureDrop
        objectName: "canvasViewDropArea"
        anchors.fill: pictureView
        keys: ["text/uri-list"]
        enabled: pictureView.visible && canvasData.layersEditable && !root.compareOn
        onEntered: drag => { drag.accepted = drag.hasUrls && drag.urls.length > 0 && drag.urls.every(url => canvasData.isPictureFile(url.toString())) }
        onDropped: drop => {
            if (!drop.hasUrls)
                return
            const k = pictureView.imageScale
            drop.accept()
            root.placeFiles(drop.urls, k > 0 ? (drop.x - pictureView.imageX) / k : -1, k > 0 ? (drop.y - pictureView.imageY) / k : -1)
        }
    }
    // History (#228, the owner's decision; Photoshop's History panel is the reference): the canvas's changes in words,
    // newest first, at the start side of the picture, closed at first. A click shows that step (read only: the Layers
    // panel says so and offers Back to latest); Up and Down move through the steps. The hash is only in the tooltip.
    Rectangle {
        id: historyPanel
        objectName: "canvasHistoryPanel"
        property bool showAll: false
        readonly property var rows: showAll ? canvasData.allSteps : canvasData.steps
        anchors.top: pictureView.top
        anchors.bottom: pictureView.bottom
        anchors.left: toolStrip.visible ? toolStrip.right : parent.left
        anchors.leftMargin: toolStrip.visible ? 2 * root.u : 0
        width: root.sideWidth
        visible: root.showHistory && canvasData.canvasId !== "" && !root.compareOn
        radius: root.look.smallCorner
        color: root.look.body
        border.width: 1
        border.color: root.look.hairline
        Column {
            anchors.fill: parent
            anchors.margins: root.u
            spacing: root.u
            Item {
                width: parent.width
                height: 7 * root.u
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: root.u
                    text: qsTr("History")
                    color: root.look.label
                    font.pixelSize: root.look.captionSize
                }
                ViewButton {
                    objectName: "canvasHistoryShowAll"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    look: root.look
                    text: qsTr("All revisions")
                    tip: qsTr("Show every revision, the ones kept apart too")
                    accented: false
                    checkable: true
                    compact: true
                    current: historyPanel.showAll
                    onTapped: historyPanel.showAll = !historyPanel.showAll
                }
            }
            ListView {
                id: historyList
                objectName: "canvasHistoryList"
                width: parent.width
                height: parent.height - y - (backLatest.visible ? backLatest.height + parent.spacing : 0)
                clip: true
                spacing: root.u / 2
                model: historyPanel.rows
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                ScrollBar.vertical: FugueScrollBar { look: root.look }
                Keys.onPressed: event => {
                    const rows = historyPanel.rows
                    const at = rows.findIndex(r => r.id === canvasData.revisionB)
                    if (event.key === Qt.Key_Down && at < rows.length - 1) canvasData.show(rows[at + 1].id)
                    else if (event.key === Qt.Key_Up && at > 0) canvasData.show(rows[at - 1].id)
                    else return
                    event.accepted = true
                }
                delegate: Rectangle {
                    id: stepRow
                    required property var modelData
                    required property int index
                    objectName: "canvasHistoryStep-" + index
                    readonly property bool shown: modelData.id === canvasData.revisionB
                    width: historyList.width
                    height: 8 * root.u
                    radius: root.look.smallCorner
                    color: shown ? root.look.selectedRow : stepHover.hovered ? root.look.menuRow : "transparent"
                    border.width: shown && historyList.activeFocus ? 2 : 0
                    border.color: root.look.accent
                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData.name
                    Accessible.selected: shown
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: root.u
                        anchors.right: stepTime.left
                        anchors.rightMargin: root.u
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: stepRow.modelData.name
                        color: stepRow.modelData.kind === "dropped" ? root.look.label : root.look.text
                        font.pixelSize: root.look.labelSize
                        font.weight: stepRow.modelData.latest ? Font.DemiBold : Font.Normal
                    }
                    Text {
                        id: stepTime
                        anchors.right: parent.right
                        anchors.rightMargin: root.u
                        anchors.verticalCenter: parent.verticalCenter
                        text: stepRow.modelData.latest ? qsTr("Latest") : stepRow.modelData.timeText.split(" ").pop()
                        color: root.look.label
                        font.pixelSize: root.look.captionSize
                        font.features: { "tnum": 1 }
                    }
                    HoverHandler { id: stepHover }
                    TapHandler {
                        onTapped: {
                            canvasData.show(stepRow.modelData.id)
                            historyList.forceActiveFocus()
                        }
                    }
                    ToolTip.visible: stepHover.hovered
                    ToolTip.delay: root.look.tooltipDelay
                    ToolTip.text: qsTr("%1, %2 · revision %3").arg(stepRow.modelData.name).arg(stepRow.modelData.timeText).arg(stepRow.modelData.id.slice(7, 15))
                }
            }
            ViewButton {
                id: backLatest
                objectName: "canvasHistoryBackLatest"
                look: root.look
                visible: canvasData.revisionB !== canvasData.head && canvasData.head !== ""
                text: qsTr("Back to latest")
                tip: qsTr("Show the latest version again")
                onTapped: canvasData.show(canvasData.head)
            }
        }
    }
    // The layers of the shown revision, at the right of the picture (the left, right to left), 2u from it.
    CanvasLayersPanel {
        id: layersPanel
        anchors.top: pictureView.top
        anchors.bottom: pictureView.bottom
        anchors.right: parent.right
        width: root.sideWidth
        visible: root.showLayers && pictureView.visible && !root.compareOn
        look: root.look
        model: canvasData
        ownShortcuts: root.ownShortcuts
        onMaskTargeted: root.paintTheMask()
        onPropertiesRequested: position => fileDialogs.askProperties(position)
        onSelectionRequested: (position, fromMask, how) => root.selectFromLayer(position, fromMask, how)
    }
    // The masks painted on the picture (#230): the targeted layer mask (its hidden part in red, while Overlay is on) and
    // the Quick Mask selection (red where painted). Under the region's frame, over the picture.
    Item {
        anchors.fill: pictureView
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        visible: pictureView.visible && !root.compareOn && !pictureView.paired && pictureView.mode !== "difference"
        MaskLayer {
            id: layerMask
            objectName: "canvasViewLayerMask"
            anchors.fill: parent
            clip: true
            visible: root.paintsMask && root.maskOverlay
            originX: pictureView.imageX
            originY: pictureView.imageY
            zoom: pictureView.imageScale
            inverted: true
            tint: Qt.alpha(root.look.error, 0.5)
            brushOpacity: root.brushOpacity
        }
        MaskLayer {
            id: selectionLayer
            objectName: "canvasViewSelection"
            anchors.fill: parent
            clip: true
            visible: root.quickMask || root.maskSelected
            imageSize: canvasData.pictureSize
            originX: pictureView.imageX
            originY: pictureView.imageY
            zoom: pictureView.imageScale
            tint: Qt.alpha(root.look.error, 0.5)
            brushOpacity: root.brushOpacity
        }
    }
    // A canvas action's live step frame, inside its selection: above the picture, under the region frame and the shimmer.
    CanvasLiveFill {
        anchors.fill: pictureView
        visible: pictureView.visible && !root.compareOn && !pictureView.paired && pictureView.mode !== "difference" && (active || opacity > 0)
        actions: App.canvasActions
        canvasId: canvasData.canvasId
        originX: pictureView.imageX
        originY: pictureView.imageY
        zoom: pictureView.imageScale
        clip: true
    }
    Item {
        id: regionLayer
        objectName: "canvasViewRegion"
        anchors.fill: pictureView
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        visible: pictureView.visible && (canvasData.region.valid === true || root.freeMode) && !pictureView.paired && pictureView.mode !== "difference"
        clip: true
        readonly property real k: pictureView.imageScale
        // The padded context box, in the picture's coordinates, which move with zoom and pan; the drag's while there is one.
        readonly property var box: root.previewReady ? root.preview.context : ({x: 0, y: 0, width: 0, height: 0})
        readonly property rect context: Qt.rect(pictureView.imageX + box.x * k, pictureView.imageY + box.y * k, box.width * k, box.height * k)
        // The rectangle: the Crop window's frame, with the picture left undimmed. It takes the left button on itself and
        // on empty picture (to draw), never the middle button, the wheel or a Space-held drag, which are the viewer's.
        Loader {
            id: regionFrame
            objectName: "canvasViewRegionFrame"
            anchors.fill: parent
            active: (root.regionValid || root.freeMode) && canvasData.pictureSize.width > 0
            sourceComponent: CropFrame {
                objectName: "canvasViewFrame"
                look: root.look
                imageSize: canvasData.pictureSize
                originX: pictureView.imageX
                originY: pictureView.imageY
                zoom: pictureView.imageScale
                rect: root.shownRegion
                dimAlpha: 0
                draw: true
                editable: root.tool === "region" && (root.regionValid ? root.regionEditable : !root.compareOn)
                yieldPress: root.panKey
                // The one snap function (GuidesModel.snapRect, canvas/snap.h): the node's Multiple of, the guides and the grid.
                constrain: root.snapOptions.multiple > 1 || root.snapOptions.guides || root.snapOptions.grid
                           ? ((r, keepSize) => guidesData.snapRect(r, keepSize, root.snapOptions)) : null
                onPressed: pictureView.forceActiveFocus()
                onChanging: r => root.liveRect = r
                onDraggingChanged: if (!dragging) root.liveRect = null
                onCommitted: r => {
                    if (root.maskSelected)
                        selectionLayer.clear() // a rectangle drawn replaces a painted selection
                    root.regionValid ? canvasData.setRegion(r) : root.placeFree(r)
                }
            }
        }
        // The padded box: the region picker's, dashes on a dark halo (ContextBox.qml).
        ContextBox {
            objectName: "canvasViewContext"
            visible: root.regionSet || root.liveRect !== null
            anchors.fill: parent
            look: root.look
            box: regionLayer.context
        }
        // Its size, small, at the box's top corner (above it when there is room, else inside); the model's size is in the
        // tooltip of the frame and under Advanced details (#234).
        Rectangle {
            id: contextLabel
            objectName: "canvasViewContextLabel"
            readonly property string text: root.previewReady ? qsTr("Context %1 × %2").arg(root.preview.context.width).arg(root.preview.context.height) : ""
            visible: (root.regionSet || root.liveRect !== null) && text !== "" && regionLayer.context.width > contextText.implicitWidth + 4 * root.u
            x: regionLayer.context.x
            y: regionLayer.context.y >= height ? regionLayer.context.y - height : regionLayer.context.y
            width: contextText.implicitWidth + root.u
            height: contextText.implicitHeight + root.u / 2
            radius: root.look.smallCorner
            color: Qt.rgba(0, 0, 0, 0.6)
            Text {
                id: contextText
                anchors.centerIn: parent
                text: contextLabel.text
                color: "white"
                font.pixelSize: root.look.captionSize
                font.features: { "tnum": 1 }
            }
        }
        // The frame's tooltip: the context and the size the model gets (hover only; it never takes the pointer).
        HoverHandler {
            id: frameHover
            objectName: "canvasViewFrameHover"
            readonly property var frame: regionFrame.item as CropFrame
            readonly property bool over: hovered && !!frame && point.position.x >= frame.fx && point.position.x <= frame.fx + frame.fw
                                         && point.position.y >= frame.fy && point.position.y <= frame.fy + frame.fh
            readonly property string text: root.previewReady
                ? qsTr("Context %1 × %2 (dashed). The model gets %3 × %4%5.")
                      .arg(root.preview.context.width).arg(root.preview.context.height)
                      .arg(root.preview.modelWidth).arg(root.preview.modelHeight)
                      .arg(root.preview.capped ? qsTr(", at the %1 MP cap").arg(canvasData.region.cap) : "") : ""
        }
        ToolTip.visible: frameHover.over && frameHover.text !== "" && root.liveRect === null
        ToolTip.delay: root.look.tooltipDelay
        ToolTip.text: frameHover.text
    }


    // The grid, the guides and the rulers (#253), over the frame: the grid under the guides, the rulers over both. Never mirrored (a
    // picture is not text). The guides take the pointer only along their own lines, with the hand and the region tools.
    Item {
        id: overlays
        objectName: "canvasViewOverlays"
        anchors.fill: pictureView
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        visible: root.overlaysShown
        readonly property real corner: root.showRulers ? root.rulerSize : 0
        // Where a guide may stay: on the picture and not under a ruler.
        readonly property rect keep: Qt.rect(Math.max(corner, pictureView.imageX), Math.max(corner, pictureView.imageY),
            Math.min(width, pictureView.imageX + canvasData.pictureSize.width * pictureView.imageScale) - Math.max(corner, pictureView.imageX),
            Math.min(height, pictureView.imageY + canvasData.pictureSize.height * pictureView.imageScale) - Math.max(corner, pictureView.imageY))
        // The guide a ruler is pulling out: {axis, at, outside}.
        property var draft: null
        function pull(axis, x, y) {
            const at = guideLines.pixelOf(axis, Qt.point(x, y))
            draft = {axis: axis, at: at, outside: !guideLines.inKeep(Qt.point(x, y))}
        }
        function drop(axis, x, y) {
            const point = Qt.point(x, y)
            const at = guideLines.pixelOf(axis, point)
            draft = null
            if (!guideLines.inKeep(point))
                return
            if (guidesData.addGuide(axis, at))
                root.showGuides = true
        }
        GridLines {
            id: gridLines
            objectName: "canvasViewGrid"
            anchors.fill: parent
            visible: root.showGrid
            look: root.look
            imageSize: canvasData.pictureSize
            originX: pictureView.imageX
            originY: pictureView.imageY
            zoom: pictureView.imageScale
            spacing: guidesData.gridSpacing
        }
        GuideLines {
            id: guideLines
            objectName: "canvasViewGuideLines"
            anchors.fill: parent
            visible: root.showGuides || overlays.draft !== null
            look: root.look
            guides: root.showGuides ? guidesData.guides.filter(g => g.at >= 0 && g.at <= (g.axis === "x" ? canvasData.pictureSize.width : canvasData.pictureSize.height)) : []
            imageSize: canvasData.pictureSize
            originX: pictureView.imageX
            originY: pictureView.imageY
            zoom: pictureView.imageScale
            draft: overlays.draft
            keep: overlays.keep
            active: root.showGuides && (root.tool === "region" || root.tool === "hand") && !root.panKey
            onGrabbed: pictureView.forceActiveFocus()
            // The model keeps its guides in the order it lists them, and so does `guides` (filtered, so the index is looked up).
            onMoved: (index, at) => guidesData.moveGuide(root.guideIndex(guides[index]), at)
            onRemoveRequested: index => guidesData.removeGuide(root.guideIndex(guides[index]))
        }
        Ruler {
            objectName: "canvasViewRulerTop"
            visible: root.showRulers
            look: root.look
            x: root.rulerSize
            width: parent.width - root.rulerSize
            height: root.rulerSize
            thickness: root.rulerSize
            origin: pictureView.imageX - root.rulerSize
            zoom: pictureView.imageScale
            extent: canvasData.pictureSize.width
            onDragStarted: (px, py) => overlays.pull("y", px, py)
            onDragMoved: (px, py) => overlays.pull("y", px, py)
            onDragEnded: (px, py) => overlays.drop("y", px, py)
        }
        Ruler {
            objectName: "canvasViewRulerLeft"
            visible: root.showRulers
            vertical: true
            look: root.look
            y: root.rulerSize
            width: root.rulerSize
            height: parent.height - root.rulerSize
            thickness: root.rulerSize
            origin: pictureView.imageY - root.rulerSize
            zoom: pictureView.imageScale
            extent: canvasData.pictureSize.height
            onDragStarted: (px, py) => overlays.pull("x", px, py)
            onDragMoved: (px, py) => overlays.pull("x", px, py)
            onDragEnded: (px, py) => overlays.drop("x", px, py)
        }
        // The corner where the rulers meet.
        Rectangle {
            visible: root.showRulers
            width: root.rulerSize
            height: root.rulerSize
            color: Qt.alpha(root.look.popup, 0.92)
        }
    }

    // The pointer of the other tools, over the region's frame (#230): the mask brush (BrushPen, the Mask editor's own),
    // the zoom's click, the hand's cursor (its drag is the viewer's pan: the frame is not edited then).
    BrushPen {
        id: brushPen
        anchors.fill: pictureView
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        visible: (root.brushing || root.lassoing) && pictureView.visible && !root.compareOn
        look: root.look
        viewer: pictureView
        target: root.paintTarget
        selects: root.lassoing
        imageSize: canvasData.pictureSize
        tool: root.tool
        brushSize: root.brushSize
        brushHardness: root.brushHardness
        spaceHeld: root.panKey
        flipErase: root.paintsMask
        prefix: "canvasViewBrush"
        onPressedAt: pictureView.forceActiveFocus()
        onResize: (size, hardness) => { root.setSize(size); root.setHardness(hardness) }
        onPainted: {
            root.selectionProblem = ""
            if (root.lassoing)
                root.lassoDone()
            else if (!root.quickMask)
                canvasData.maskPainted()
        }
    }
    MouseArea {
        objectName: "canvasViewZoomTool"
        anchors.fill: pictureView
        visible: root.tool === "zoom" && pictureView.visible && !root.panKey
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.CrossCursor
        onClicked: mouse => {
            pictureView.forceActiveFocus()
            const out = ((mouse.modifiers & Qt.AltModifier) !== 0) !== root.zoomOut
            pictureView.zoomAt(mouse.x, mouse.y, out ? 0.8 : 1.25)
        }
    }
    Item {
        anchors.fill: pictureView
        visible: root.tool === "hand" && pictureView.visible
        HoverHandler { cursorShape: Qt.OpenHandCursor }
    }

    // The Move tool and Free Transform (#236): the layer's box over the picture; the numbers are TransformGesture's.
    TransformGesture {
        id: xfGesture
        guides: guidesData
        snapOptions: root.snapOptions
    }
    TransformFrame {
        id: xfFrame
        objectName: "canvasViewTransformFrame"
        anchors.fill: pictureView
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        look: root.look
        gesture: xfGesture
        placement: root.xfPlacement
        imageSource: canvasData.transformImage
        opacityOfLayer: canvasData.transformOpacity
        originX: pictureView.imageX
        originY: pictureView.imageY
        zoom: pictureView.imageScale
        handles: root.freeTransform
        moveAnywhere: root.tool === "move" && !root.freeTransform
        yieldPress: root.panKey
        visible: pictureView.visible && !root.compareOn && (root.xfOpen || (root.tool === "move" && !root.freeTransform))
        onAboutToPress: { pictureView.forceActiveFocus(); root.beginTransformSession() }
        onChangeRequested: placement => root.xfPlacement = placement
        onDragEnded: committed => {
            if (!root.freeTransform) {
                if (committed)
                    root.commitTransform()
                else
                    root.endTransformSession()
            }
        }
    }
    // Free Transform's keys (Enter keeps it, Escape leaves it, the arrows move the layer a pixel, with Shift ten) are the view's own key
    // handlers above (Keys.onShortcutOverride and Keys.onPressed), not Shortcut objects: a dozen of those made while the view is made upset
    // the creation of the dialogs under it, which then failed under load with "Cannot find member data".
    // What the box was made for goes away with it: another layer picked, another canvas, a change of the Head.
    Connections {
        target: root.layersPanel
        function onCurrentChanged() {
            if (root.xfOpen && (root.layersPanel.current === null || root.layersPanel.current.position !== root.xfPosition))
                root.endTransformSession()
        }
    }
    Connections {
        target: canvasData
        function onCanvasChanged() { if (root.xfOpen) root.endTransformSession() }
        function onTransformingChanged() { if (!canvasData.transforming && root.xfOpen) root.endTransformSession() }
    }
    // A run's layer landed on the Head while the view was on it (the normal flow, #228): the view went along; a short
    // notice says what came, with Undo (the layers panel's: the Head back one step) and a close. No decision to take.
    Banner {
        id: headToast
        objectName: "canvasViewHeadToast"
        anchors.left: pictureView.left
        anchors.right: pictureView.right
        anchors.bottom: pictureView.bottom
        anchors.margins: 2 * root.u
        anchors.bottomMargin: 8 * root.u
        visible: canvasData.headNotice !== "" && pictureView.visible
        look: root.look
        Timer {
            running: headToast.visible
            interval: 10000
            onTriggered: canvasData.clearHeadNotice()
        }
        Item {
            width: parent.width
            height: toastUndo.height
            Text {
                objectName: "canvasViewHeadToastText"
                anchors.left: parent.left
                anchors.right: toastUndo.left
                anchors.rightMargin: 2 * root.u
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideRight
                color: root.look.text
                font.pixelSize: root.look.labelSize
                text: canvasData.headNotice
            }
            ViewButton {
                id: toastUndo
                objectName: "canvasViewHeadToastUndo"
                anchors.right: toastClose.left
                anchors.rightMargin: root.u
                anchors.verticalCenter: parent.verticalCenter
                look: root.look
                glyph: "undo-2"
                text: qsTr("Undo")
                enabled: canvasData.canUndo
                tip: qsTr("Take the canvas back one step (%1)").arg((App.commands.entry("canvas-layers.undo").keys ?? []).join(", "))
                onTapped: canvasData.undo()
            }
            ViewButton {
                id: toastClose
                objectName: "canvasViewHeadToastClose"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                look: root.look
                glyph: "x"
                flat: true
                compact: true
                tip: qsTr("Close")
                onTapped: canvasData.clearHeadNotice()
            }
        }
    }

    // The task bar (#231, CanvasTaskBar.qml): under the selection, Generate into it on the latest version.
    CanvasTaskBar {
        id: taskBar
        anchors.fill: pictureView
        look: root.look
        visible: shown && pictureView.visible && !root.compareOn
        canvasId: canvasData.canvasId
        revision: canvasData.revisionB
        onHead: root.shownIsHead
        regionLayer: canvasData.regionLayer
        currentLayer: canvasData.currentLayer
        rect: root.shownRegion
        quickMask: root.maskSelected ? selectionLayer : null
        originX: pictureView.imageX
        originY: pictureView.imageY
        zoom: pictureView.imageScale
        roomX: -pictureView.x
        roomWidth: root.width
        onBackToLatest: canvasData.show(canvasData.head)
        // Edit workflow opened a tab: the Graph's area takes the focus; the dialog, which hides it, closes.
        onWorkflowOpened: {
            if (root.hostCloses)
                root.closeRequested()
            else if (App.workspace)
                App.workspace.active = App.workspace.graphArea
        }
    }

    // What the picture area says when there is nothing to show, and what to do about it in the app.
    Column {
        objectName: "canvasViewEmpty"
        anchors.centerIn: pictureView
        width: Math.min(pictureView.width - 4 * root.u, 120 * root.u)
        visible: !pictureView.visible
        spacing: 2 * root.u
        Text {
            objectName: "canvasViewEmptyText"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: canvasData.canvasId !== "" && canvasData.error !== "" ? root.look.error : root.look.label
            font.pixelSize: root.look.labelSize
            text: canvasData.canvasId === "" ? (canvasData.canvases.length === 0 ? qsTr("No canvases yet") : qsTr("Choose a canvas"))
                                          : canvasData.error !== "" ? canvasData.error : qsTr("This canvas has no revision yet.")
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: canvasData.canvasId === ""
            spacing: root.u
            ViewButton {
                objectName: "canvasViewEmptyNew"
                look: root.look
                text: qsTr("New canvas…")
                tip: qsTr("Make a blank canvas, or one from a picture")
                onTapped: root.newCanvas()
            }
            ViewButton {
                objectName: "canvasViewEmptyImport"
                look: root.look
                text: qsTr("Import picture…")
                tip: qsTr("Make a canvas from a picture file")
                onTapped: root.importPicture()
            }
        }
        Text {
            objectName: "canvasViewMakeProblem"
            width: parent.width
            visible: root.makeProblem !== ""
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: root.look.error
            font.pixelSize: root.look.labelSize
            text: root.makeProblem
        }
    }

    Column {
        id: notes
        anchors.bottom: statusLine.top
        width: parent.width
        spacing: root.u
        // The followed Canvas Region reads another canvas, or a linked one: said plainly, with what can be done about it
        // (never a dead end in the error color).
        Banner {
            objectName: "canvasViewCanvasMismatch"
            width: parent.width
            visible: canvasData.followNode !== "" && (canvasData.region.problem === "otherCanvas" || canvasData.region.problem === "linked")
            look: root.look
            Column {
                width: parent.width
                spacing: root.u
                Text {
                    objectName: "canvasViewCanvasMismatchText"
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: root.look.text
                    font.pixelSize: root.look.labelSize
                    text: canvasData.region.reason ?? ""
                }
                Flow {
                    width: parent.width
                    spacing: root.u
                    visible: canvasData.region.problem === "otherCanvas"
                    ViewButton {
                        objectName: "canvasViewShowNodeCanvas"
                        look: root.look
                        visible: canvasData.region.known === true
                        text: qsTr("Show %1").arg(canvasData.region.nodeCanvasName ?? "")
                        tip: qsTr("Look at the canvas this node reads")
                        onTapped: canvasData.showNodeCanvas()
                    }
                    ViewButton {
                        objectName: "canvasViewUseCanvasForNode"
                        look: root.look
                        text: qsTr("Use %1 for this node").arg(canvasButton.text)
                        tip: qsTr("Make %1 read the canvas shown (one undo step)").arg(root.followName)
                        onTapped: canvasData.useCanvasForNode()
                    }
                    ViewButton {
                        objectName: "canvasViewUseActiveForNode"
                        look: root.look
                        text: qsTr("Use the active canvas")
                        tip: qsTr("Make %1 follow the canvas you work on in the Canvas view, not this one (one undo step)").arg(root.followName)
                        onTapped: canvasData.useActiveCanvasForNode()
                    }
                }
            }
        }
        // The followed Canvas Region: what it reads, and the one action on it.
        Flow {
            id: regionRow
            objectName: "canvasViewRegionRow"
            width: parent.width
            spacing: 2 * root.u
            visible: canvasData.followNode !== "" && canvasData.region.problem !== "otherCanvas" && canvasData.region.problem !== "linked"
                     && (root.pinnedElsewhere || canvasData.region.valid !== true)
            Text {
                objectName: "canvasViewRegionNote"
                readonly property var shown: root.previewReady ? root.preview : canvasData.region // the saved numbers until the picture is read
                height: useButton.height
                verticalAlignment: Text.AlignVCenter
                color: canvasData.region.valid ? root.look.label : root.look.error
                font.pixelSize: root.look.labelSize
                text: canvasData.region.valid ? qsTr("%1 reads %2: %3 × %4 at %5, %6").arg(root.followName).arg(root.readsText(canvasData.region))
                                                  .arg(shown.width).arg(shown.height).arg(shown.x).arg(shown.y)
                                           : canvasData.region.reason
            }
            ViewButton {
                id: useButton
                objectName: "canvasViewUseRegion"
                look: root.look
                visible: canvasData.region.valid === true
                enabled: canvasData.region.writable === true && canvasData.region.readsShown !== true && canvasData.revisionB !== ""
                text: qsTr("Use for region")
                tip: canvasData.region.writable !== true ? qsTr("The Revision input is linked to another node: change it there")
                     : canvasData.region.readsShown === true ? qsTr("The region reads this revision already")
                     : root.shownIsHead ? qsTr("Make the region read the latest version, whichever step that is") : qsTr("Make the region read exactly this step")
                onTapped: canvasData.useForRegion()
            }
        }
        // Only what a person must act on: why a drag does nothing (linked, Compare), or what to do with a drawn rectangle.
        // How to pick is the tool strip's tooltips (#234).
        Text {
            objectName: "canvasViewRegionHint"
            width: parent.width
            wrapMode: Text.Wrap
            visible: pictureView.visible && text !== "" && (root.freeMode || root.regionValid)
            color: root.regionValid && !root.compareOn && !root.regionEditable ? root.look.error : root.look.label
            font.pixelSize: root.look.labelSize
            text: root.freeMode ? (root.compareOn ? qsTr("Turn Compare off to draw a region on the picture.")
                                   : root.hasFreeRect ? (root.regionsHere.length > 1 ? qsTr("This rectangle is not a node's yet: choose its Canvas Region.")
                                                         // With the task bar (#231) the rectangle is used as it is: nothing to say.
                                                         : App.canvasActions !== null ? ""
                                                         : qsTr("This rectangle is not saved: no Canvas Region node reads this canvas. Add one to use it."))
                                   : "")
                  : root.regionEditable ? ""   // how to pick is taught by the tool strip's tooltips (#234)
                  : root.compareOn ? qsTr("Turn Compare off to pick the region on the picture.")
                  : qsTr("X, Y, Width or Height is linked to another node: pick it there.")
        }
        // What a drawn rectangle can become: a node's, or a new node.
        Flow {
            objectName: "canvasViewFreeRow"
            width: parent.width
            spacing: root.u
            visible: root.hasFreeRect && !root.compareOn
            ViewButton {
                objectName: "canvasViewAddRegion"
                look: root.look
                visible: root.regionsHere.length === 0
                enabled: canvasData.edits !== null
                text: qsTr("Add a Canvas Region node")
                tip: qsTr("Add a Canvas Region node that reads this canvas, with this rectangle (one undo step)")
                onTapped: root.addRegionNode()
            }
            ViewButton {
                objectName: "canvasViewChooseRegion"
                look: root.look
                visible: root.regionsHere.length > 1
                text: qsTr("Choose its node…")
                tip: qsTr("Which Canvas Region node this rectangle is for")
                onTapped: root.chooseRegion()
            }
        }
        Text {
            objectName: "canvasViewNotice"
            width: parent.width
            wrapMode: Text.Wrap
            visible: canvasData.notice !== ""
            color: root.look.label
            font.pixelSize: root.look.labelSize
            text: canvasData.notice
        }
        Text {
            objectName: "canvasViewError"
            width: parent.width
            wrapMode: Text.Wrap
            visible: (canvasData.error !== "" && pictureView.visible) || (root.makeProblem !== "" && pictureView.visible)
            color: root.look.error
            font.pixelSize: root.look.labelSize
            text: canvasData.error !== "" ? canvasData.error : root.makeProblem
        }
    }

    // Photoshop's status line: one thin row under the picture. Zoom and the canvas size at the start, the selection's size
    // and position (live while dragging) next to it, and Advanced details at the end for the numbers a film team wants
    // (#234). Nothing else is written under the picture unless a person must act on it.
    Column {
        id: statusLine
        objectName: "canvasViewStatusLine"
        anchors.bottom: parent.bottom
        width: parent.width
        visible: pictureView.visible
        height: visible ? implicitHeight : 0
        spacing: 0
        Text {
            objectName: "canvasViewStatusDetails"
            width: parent.width
            visible: root.advancedDetails
            wrapMode: Text.Wrap
            color: root.look.label
            font.pixelSize: root.look.captionSize
            font.features: { "tnum": 1 }
            text: root.detailsText
        }
        Item {
            width: parent.width
            height: statusRow.implicitHeight + root.u
            Row {
                id: statusRow
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: root.u
                spacing: 3 * root.u
                Text {
                    objectName: "canvasViewStatusZoom"
                    color: root.look.label
                    font.pixelSize: root.look.captionSize
                    font.features: { "tnum": 1 }
                    text: qsTr("%1 · %2 × %3").arg(pictureView.zoomText).arg(canvasData.pictureSize.width).arg(canvasData.pictureSize.height)
                }
                Text {
                    objectName: "canvasViewStatusSelection"
                    visible: root.selectionRect !== null && root.selectionRect.width > 0
                    color: root.look.text
                    font.pixelSize: root.look.captionSize
                    font.features: { "tnum": 1 }
                    text: root.selectionRect ? qsTr("%1 × %2 at %3, %4").arg(Math.round(root.selectionRect.width)).arg(Math.round(root.selectionRect.height))
                                                .arg(Math.round(root.selectionRect.x)).arg(Math.round(root.selectionRect.y)) : ""
                }
            }
            ViewButton {
                objectName: "canvasViewAdvancedDetails"
                anchors.right: parent.right
                anchors.rightMargin: root.u
                anchors.verticalCenter: parent.verticalCenter
                look: root.look
                text: qsTr("Advanced details")
                flat: true
                compact: true
                accented: false
                checkable: true
                current: root.advancedDetails
                tip: qsTr("The context and model size, the revision and what changed in it")
                onTapped: root.advancedDetails = !root.advancedDetails
            }
        }
    }
}
