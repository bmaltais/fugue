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
// rectangle. It edits whichever revision is shown, a candidate included; with X, Y, Width or Height linked to another
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
// adds the candidates for those who need them. Candidates are rare now (runs land on the latest version): one chip
// "N pending" opens the newest for Accept or Drop. A candidate shows over the latest version; Show candidate switches it
// off to see the latest alone. Compare (against the latest for a candidate, else the step before) opens
// the compare modes of Compare Images (CompareModeButtons.qml). One bar (#228): the canvas and the pending chip, then the
// tools in chunks (History and Layers, the look, Export, the decision, Drop, Fit and 100%) and the host's `trailing` items
// last (the dialog's close). A run's layer that lands on the latest version is followed, with a short notice "Layer
// <name> added" and Undo. A real candidate (a Canvas Write with Keep as candidate on, or one that could not land on a
// moved latest version) shows at once under a banner over the picture's foot (it never moves the picture): Back to
// latest, Accept and Drop; the bar's
// own Accept and Drop hide meanwhile, so one primary is on screen.
// Keys (docs/ui-ergonomics.md D5, one keymap with the Graph): Home or 0 Fit, 1 100%, + and - zoom, Enter accepts the
// shown candidate (when the focus is in no field and no button), Escape ends a drag, Space held pans. While it has the
// focus the window's bare Q (quit) does not fire here: it is kept for a quick mask (docs/ui-ergonomics.md References);
// Ctrl+Q and the close button still quit, and the Graph's keys sleep while a Canvas area is the active one (Workspace).
// Right to left the bar, the notes and the layers panel mirror with the window; the picture never does.
//
// The tool strip (#230; Photoshop's toolbar, Blender's toolbar: docs/ui-ergonomics.md References) is a narrow column at
// the start of the picture, one icon button per tool with its key in the tooltip, the current one in the quiet marking:
// Hand (H; Space held for a moment), Zoom (Z; Alt zooms out), Rectangle region (M or R: the frame above, the default),
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
    // A node with no canvas yet counts as this canvas's: it takes it at the first edit.
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
    property bool panKey: false // Space held: a left drag pans, as the middle button does
    // Why the last New canvas… or Import picture… made nothing (the error line says it).
    property string makeProblem: ""
    readonly property bool regionValid: canvasData.region.valid === true
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
    readonly property bool canDecide: canvasData.kindB === "candidate" || canvasData.kindB === "dropped"
    readonly property bool shownIsHead: canvasData.revisionB !== "" && canvasData.revisionB === canvasData.head
    // The revisions shown, as CanvasHistory names them ({name, tag, kind, ...}; {} before any).
    readonly property var rowB: canvasData.revisions.find(row => row.id === canvasData.revisionB) ?? ({})
    readonly property var rowA: canvasData.revisions.find(row => row.id === canvasData.revisionA) ?? ({})
    // The followed node as a sentence names it: its title (WidgetEdit::nodeName), never its id alone.
    readonly property string followName: canvasData.followNode === "" ? "" : (canvasData.edits ? canvasData.edits.nodeName(canvasData.followNode) : "")
                                                                             || canvasData.followNode

    function fit() { pictureView.fit() }
    function actualSize() { pictureView.actualSize() }
    // Reads the store again now (a host shows it again after it was away).
    function refresh() { canvasData.refresh() }
    // Enter: Accept, when a candidate is shown and its Accept is on screen (the bar's or the banner's).
    function acceptShown() {
        if (!canDecide)
            return false
        canvasData.accept(canvasData.revisionB)
        return true
    }
    // What a person calls the revision shown (no hashes, #228): the latest version, a candidate, or the step it is.
    function labelOf(row) {
        if (row.kind === undefined)
            return ""
        return row.kind === "head" ? qsTr("Latest") : row.kind === "candidate" ? qsTr("Candidate")
             : row.kind === "dropped" ? qsTr("Dropped candidate") : canvasData.stepName(row.id)
    }
    // The pending chip: the newest open candidate, shown for its review (Accept, Drop in the bar).
    function reviewPending() {
        const row = canvasData.revisions.find(r => r.kind === "candidate")
        if (row)
            canvasData.show(row.id)
    }
    property bool showHistory: false

    // ---- the tool strip and its options (#230) ----
    property string tool: "region" // hand, zoom, region, brush, eraser
    property bool quickMask: false
    property bool zoomOut: false
    property real brushSize: 64
    property real brushHardness: 0.8
    property real brushOpacity: 1
    property bool maskOverlay: true
    // Why the last Use selection did nothing (the options bar says it; a stroke clears it).
    property string selectionProblem: ""
    readonly property bool brushing: tool === "brush" || tool === "eraser"
    readonly property real maxBrush: Math.max(64, Math.min(2048, Math.max(canvasData.pictureSize.width, canvasData.pictureSize.height)))
    readonly property bool paintsMask: !quickMask && canvasData.maskTarget >= 0
    readonly property var maskRow: canvasData.layers.find(r => r.position === canvasData.maskTarget) ?? null
    readonly property var paintTarget: quickMask ? selectionLayer : paintsMask ? layerMask : null
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
        canvasData.commitMask() // the strokes so far are their own step
        tool = name
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
    // What Deselect can clear now: the free rectangle, the node's area (when its numbers are not linked or compared), the painted selection.
    readonly property bool deselectable: (rectSelected && (!regionValid || regionEditable)) || maskSelected
    // Escape clears the selection when nothing else wants it: no drag, no run of the task bar or the Graph.
    readonly property bool escapeDeselects: deselectable && !taskBar.busy && !dragging && App.commands.entry("run.cancel").enabled !== true
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
        case "canvas-view.export": return {enabled: canvasData.revisionB !== ""}
        case "canvas-view.rename": case "canvas-view.duplicate": case "canvas-view.delete": case "canvas-view.close": return {enabled: has}
        case "canvas-view.clear": return {enabled: canvasData.layersEditable}
        case "canvas-view.select-all": return {enabled: shows && canvasData.pictureSize.width > 0}
        case "canvas-view.deselect": return {enabled: deselectable}
        case "canvas-view.invert-selection": return {enabled: quickMask}
        case "canvas-view.use-selection": return {enabled: quickMask && regionValid && canvasData.region.selectionWritable === true}
        case "canvas-view.quick-mask": return {enabled: has, checkable: true, checked: quickMask}
        case "canvas-view.add-mask": return {enabled: canvasData.layersEditable && layersPanel.current !== null && layersPanel.current.kind !== "mask"}
        case "canvas-view.mask-invert": return {enabled: quickMask || paintsMask}
        case "canvas-view.mask-overlay": return {enabled: paintsMask, checkable: true, checked: maskOverlay}
        case "canvas-view.mask-reveal": case "canvas-view.mask-hide": return {enabled: paintsMask}
        case "canvas-view.undo": return {enabled: !quickMask || selectionLayer.canUndo}
        case "canvas-view.redo": return {enabled: !quickMask || selectionLayer.canRedo}
        // The keys the view handles by hand are shown as text (they are not bound twice).
        case "canvas-view.fit": return {enabled: shows, keys: ["Home"]}
        case "canvas-view.actual-size": return {enabled: shows, keys: ["1"]}
        case "canvas-view.zoom-in": return {enabled: shows, keys: ["+"]}
        case "canvas-view.zoom-out": return {enabled: shows, keys: ["-"]}
        case "canvas-view.history": return {enabled: has, checkable: true, checked: showHistory}
        case "canvas-view.layers": return {enabled: has, checkable: true, checked: showLayers}
        case "canvas-view.compare": return {enabled: canvasData.hasParent, checkable: true, checked: compareOn}
        case "canvas-view.show-candidate": return {enabled: canvasData.showsCandidate && !compareOn, checkable: true, checked: canvasData.overlay}
        case "canvas-view.details": return {enabled: has, checkable: true, checked: advancedDetails}
        case "canvas-view.snap": return {enabled: root.tool === "region" && regionEditable && canvasData.region.multiple > 1, checkable: true, checked: snap}
        case "canvas-view.tool-hand": return tool("hand")
        case "canvas-view.tool-zoom": return tool("zoom")
        case "canvas-view.tool-region": return tool("region")
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
    // A command of the strip (the registry's `canvas-view.*`), from a key or a menu.
    function runTool(id) {
        switch (id) {
        case "canvas-view.tool-hand": setTool("hand"); break
        case "canvas-view.tool-zoom": setTool("zoom"); break
        case "canvas-view.tool-region": setTool("region"); break
        case "canvas-view.tool-brush": setTool("brush"); break
        case "canvas-view.tool-eraser": setTool("eraser"); break
        case "canvas-view.brush-swap": if (brushing) tool = tool === "brush" ? "eraser" : "brush"; break
        case "canvas-view.brush-smaller": stepSize(false); break
        case "canvas-view.brush-larger": stepSize(true); break
        case "canvas-view.quick-mask": toggleQuickMask(); break
        case "canvas-view.add-mask": addMaskToCurrent(); break
        case "canvas-view.mask-overlay": maskOverlay = !maskOverlay; break
        case "canvas-view.mask-invert": if (quickMask) selectionLayer.invert(); else canvasData.invertMask(); break
        // Undo follows what is being done: a Quick Mask stroke, a layer mask's revision, else the document's (the region).
        case "canvas-view.new": newCanvas(); break
        case "canvas-view.open": chooseCanvas(); break
        case "canvas-view.import": importPicture(); break
        case "canvas-view.export": exportDialogOpen(); break
        case "canvas-view.rename": fileDialogs.askRename(); break
        case "canvas-view.duplicate": canvasData.duplicateCanvas(); break
        case "canvas-view.clear": fileDialogs.askClear(); break
        case "canvas-view.delete": fileDialogs.askDelete(); break
        case "canvas-view.close": closeCanvas(); break
        case "canvas-view.select-all": selectAll(); break
        case "canvas-view.deselect": deselect(); break
        case "canvas-view.invert-selection": if (quickMask) selectionLayer.invert(); break
        case "canvas-view.use-selection": if (quickMask) useSelection(); break
        case "canvas-view.mask-reveal": canvasData.fillMask(); break
        case "canvas-view.mask-hide": canvasData.clearMask(); break
        case "canvas-view.fit": fit(); break
        case "canvas-view.actual-size": actualSize(); break
        case "canvas-view.zoom-in": pictureView.zoomAt(pictureView.width / 2, pictureView.height / 2, 1.25); break
        case "canvas-view.zoom-out": pictureView.zoomAt(pictureView.width / 2, pictureView.height / 2, 0.8); break
        case "canvas-view.history": showHistory = !showHistory; break
        case "canvas-view.layers": showLayers = !showLayers; break
        case "canvas-view.compare": comparing = !comparing; break
        case "canvas-view.show-candidate": canvasData.overlay = !canvasData.overlay; break
        case "canvas-view.details": advancedDetails = !advancedDetails; break
        case "canvas-view.snap": snap = !snap; break
        case "canvas-view.undo":
            if (quickMask) selectionLayer.undo()
            else if (paintsMask || canvasData.maskPending) canvasData.undo()
            else App.commands.trigger("edit.undo")
            break
        case "canvas-view.redo":
            if (quickMask) selectionLayer.redo()
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
        }
    }
    // Escape clears the selection wherever the focus is, as Ctrl+D does (a key to a focused item would miss it after a click
    // elsewhere in the dialog); `escapeDeselects` is false while a drag or a run wants the key.
    Shortcut {
        sequence: "Escape"
        enabled: root.keysHot && root.escapeDeselects
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

    CanvasViewModel {
        id: canvasData
        onCanvasChanged: { root.freeRect = root.noRect; selectionLayer.reset() }
        maskLayer: layerMask
        onFollowChanged: root.freeRect = root.noRect
        // A run's new candidate is shown at once (settings.json's canvas_show_new_results, live), unless the view is busy:
        // Compare on, or a rectangle being dragged. Then it waits, and shows when that ends (if still on the Head).
        showNewResults: App.settings ? App.settings.canvasShowNewResults : true
        viewLocked: root.compareOn || root.dragging
    }
    // Delayed: following a node moves the view to its canvas, which changes the nodes that read the canvas shown.
    Binding {
        target: canvasData
        property: "followNode"
        value: root.followTarget
        delayed: true
    }
    readonly property bool dragging: (regionFrame.item as CropFrame)?.dragging ?? false
    // The banner of a candidate the view moved to by itself: only while that candidate is the one shown and open.
    readonly property bool newResultShown: canvasData.newResult !== "" && canvasData.revisionB === canvasData.newResult && canvasData.kindB === "candidate"

    Component.onCompleted: {
        // One canvas by default: with none chosen, the newest.
        if (canvasData.canvasId === "" && canvasData.canvases.length > 0)
            canvasData.canvasId = canvasData.canvases[0].id
    }
    onCompareOnChanged: pictureView.setView({mode: compareOn ? modeButtons.remembered() : "single"})

    // The picture is saved by the file dialog, as Save Image does: the model writes it and never replaces a file.
    FilePicker {
        id: exportDialog
        objectName: "canvasViewExportDialog"
        title: qsTr("Save the revision as a picture")
        fileMode: PortalFileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: [qsTr("PNG images (*.png)"), qsTr("JPEG images (*.jpg *.jpeg)"), qsTr("WebP images (*.webp)"), qsTr("Layered image (*.ora)")]
        onAccepted: canvasData.exportPicture(selectedFile.toString())
    }
    CanvasFileDialogs {
        id: fileDialogs
        view: root
    }
    Component.onDestruction: {
        exportDialog.close()
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
    Component {
        id: newCanvasComponent
        NewCanvasDialog {
            parent: Overlay.overlay
            onCreated: id => {
                root.makeProblem = ""
                canvasData.useCanvas(id)
            }
        }
    }
    Component {
        id: pictureDialog
        FilePicker {
            id: d
            objectName: "canvasViewImportDialog"
            title: qsTr("Import a picture as a canvas")
            fileMode: PortalFileDialog.OpenFile
            nameFilters: [qsTr("Pictures and canvases (*.png *.jpg *.jpeg *.webp *.ora)"), qsTr("All files (*)")]
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
            event.accepted = root.dragging || taskBar.busy || root.escapeDeselects // a drag's, the task bar's Cancel, or Deselect
            return
        }
        const owner = App.commands.ownerOfPress(event.key, event.modifiers)
        // The bare Q is Quick Mask's here (#230, the `canvas-view` context); it never quits from the view.
        event.accepted = (event.key === Qt.Key_Q && bare && !owner.startsWith("canvas-view.")) || (owner !== "" && App.commands.actsOnGraph(owner))
    }
    Keys.onPressed: event => {
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
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (!acceptShown())
                return // nothing to accept: the host's (a dialog's confirm)
        }
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

    // One bar (#228, the owner's decision: no strip of revisions, no hashes): the canvas, the pending candidates when
    // there are any, the History and Layers panels, then the tools. It wraps when the view is narrow.
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
            // Candidates waiting (rare: a Canvas Write with Keep as candidate on, or one that could not land on the
            // latest version): one chip; nothing when there are none.
            ViewButton {
                objectName: "canvasViewPending"
                look: root.look
                visible: canvasData.pendingCount > 0
                text: qsTr("%n pending", "", canvasData.pendingCount)
                tip: qsTr("Review the candidates waiting: Accept or Drop")
                accented: false
                current: root.canDecide
                onTapped: root.reviewPending()
            }
        }
        // How the shown revision is looked at: over the latest version or alone, or against another (the modes).
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
        // The decision, hidden while the banner shows its own (one primary on screen, docs/ui-ergonomics.md Primary).
        Chunk {
            visible: root.canDecide && !root.newResultShown
            ViewButton {
                objectName: "canvasViewAccept"
                look: root.look
                primary: true
                text: qsTr("Accept")
                tip: qsTr("Make this candidate the latest version of the canvas (Enter)")
                onTapped: canvasData.accept(canvasData.revisionB)
            }
        }
        // The destructive chunk, apart.
        Chunk {
            visible: canvasData.kindB === "candidate" && !root.newResultShown
            ViewButton {
                objectName: "canvasViewDrop"
                look: root.look
                danger: true
                text: qsTr("Drop")
                tip: qsTr("Hide this candidate (it is kept, and Accept still takes it)")
                onTapped: canvasData.drop(canvasData.revisionB)
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
            text: root.quickMask && root.brushing ? qsTr("Quick Mask") : root.tool === "hand" ? qsTr("Hand") : root.tool === "zoom" ? qsTr("Zoom")
                  : root.tool === "brush" ? qsTr("Mask brush") : root.tool === "eraser" ? qsTr("Mask eraser") : qsTr("Rectangle region")
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
            ToolButton { toolName: "region"; command: "canvas-view.tool-region"; glyph: "square-dashed"; name: qsTr("Rectangle region: drag the area a run redraws") }
            Item { width: 1; height: 2 * root.u }
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
        onActiveFocusChanged: if (!activeFocus) root.panKey = false
        mode: "single"
        showZoom: true
        showSize: false // the status line under the picture says the size and the zoom
        // Alone, the picture is what the overlay says (a candidate over the Head, the Head, a revision); compared, A and B.
        // Each named as CanvasHistory names it.
        sourceA: root.compareOn ? canvasData.sourceA : canvasData.pictureSource
        sourceB: canvasData.sourceB
        sourceDifference: canvasData.sourceDifference
        sizeA: root.compareOn ? canvasData.sizeA : canvasData.pictureSize
        sizeB: canvasData.sizeB
        labelA: root.compareOn ? root.labelOf(root.rowA) : canvasData.showsCandidate && !canvasData.overlay ? qsTr("Latest") : root.labelOf(root.rowB)
        labelB: root.labelOf(root.rowB)
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
        width: 56 * root.u
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
                    tip: qsTr("Show all revisions: the candidates too, pending and dropped")
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
        visible: root.showLayers && pictureView.visible && !root.compareOn
        look: root.look
        model: canvasData
        ownShortcuts: root.ownShortcuts
        decisionElsewhere: root.newResultShown
        onMaskTargeted: root.paintTheMask()
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
            visible: root.quickMask
            imageSize: canvasData.pictureSize
            originX: pictureView.imageX
            originY: pictureView.imageY
            zoom: pictureView.imageScale
            tint: Qt.alpha(root.look.error, 0.5)
            brushOpacity: root.brushOpacity
        }
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
                constrain: root.snap && root.regionValid ? ((r, keepSize) => canvasData.snapRegion(r, keepSize)) : null
                onPressed: pictureView.forceActiveFocus()
                onChanging: r => root.liveRect = r
                onDraggingChanged: if (!dragging) root.liveRect = null
                onCommitted: r => root.regionValid ? canvasData.setRegion(r) : root.placeFree(r)
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
            readonly property bool over: hovered && frame !== null && point.position.x >= frame.fx && point.position.x <= frame.fx + frame.fw
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


    // The pointer of the other tools, over the region's frame (#230): the mask brush (BrushPen, the Mask editor's own),
    // the zoom's click, the hand's cursor (its drag is the viewer's pan: the frame is not edited then).
    BrushPen {
        id: brushPen
        anchors.fill: pictureView
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        visible: root.brushing && pictureView.visible && !root.compareOn
        look: root.look
        viewer: pictureView
        target: root.paintTarget
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
            if (!root.quickMask)
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

    // A run made a new candidate and the view moved to it (it shows over the Head): what to do with it, in one place, over
    // the foot of the picture so the picture does not move under the pointer.
    Banner {
        id: banner
        objectName: "canvasViewNewResult"
        anchors.left: pictureView.left
        anchors.right: pictureView.right
        anchors.bottom: pictureView.bottom
        anchors.margins: 2 * root.u
        anchors.bottomMargin: 8 * root.u // the picture's size and zoom notice stays readable under it
        visible: root.newResultShown
        look: root.look
        decision: true
        Column {
            width: parent.width
            spacing: root.u
            Text {
                objectName: "canvasViewNewResultText"
                width: parent.width
                wrapMode: Text.Wrap
                color: root.look.text
                font.pixelSize: root.look.labelSize
                text: (canvasData.newResultFrom !== "" ? qsTr("A new candidate from %1 is shown over the latest version.").arg(canvasData.newResultFrom)
                                                         : qsTr("A new candidate is shown over the latest version."))
                      + (canvasData.newResultOthers > 0 ? " " + qsTr("%n more candidates are pending.", "", canvasData.newResultOthers) : "")
            }
            // Back first, the primary, then Drop in a chunk of its own last (docs/ui-ergonomics.md Order and Destructive).
            Flow {
                width: parent.width
                spacing: root.u
                ViewButton {
                    objectName: "canvasViewNewBack"
                    look: root.look
                    text: qsTr("Back to latest")
                    tip: qsTr("Show the latest version; the candidate stays pending")
                    onTapped: canvasData.show(canvasData.head)
                }
                Chunk {
                    ViewButton {
                        objectName: "canvasViewNewAccept"
                        look: root.look
                        primary: true
                        text: qsTr("Accept")
                        tip: qsTr("Make this candidate the latest version of the canvas (Enter)")
                        onTapped: canvasData.accept(canvasData.revisionB)
                    }
                }
                Chunk {
                    ViewButton {
                        objectName: "canvasViewNewDrop"
                        look: root.look
                        danger: true
                        text: qsTr("Drop")
                        tip: qsTr("Hide this candidate (it is kept, and Accept still takes it)")
                        onTapped: canvasData.drop(canvasData.revisionB)
                    }
                }
            }
        }
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
        visible: canvasData.headNotice !== "" && !root.newResultShown && pictureView.visible
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
        onHead: root.shownIsHead && !canvasData.showsCandidate
        rect: root.shownRegion
        quickMask: root.quickMask ? selectionLayer : null
        originX: pictureView.imageX
        originY: pictureView.imageY
        zoom: pictureView.imageScale
        roomX: -pictureView.x
        roomWidth: root.width
        decisionElsewhere: root.newResultShown
        onBackToLatest: canvasData.show(canvasData.head)
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
                  : root.regionEditable && canvasData.region.adopts === true ? qsTr("Drag on the picture to pick the region: this Canvas Region has no canvas yet and takes this one.")
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
