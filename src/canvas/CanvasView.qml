pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import Fugue
import "widgets"
import "canvasMenu.js" as CanvasMenu

// The Canvas view (#221, epics #216 and #217, spec 14.5): one canvas as a picture you can look at, choose a revision of,
// decide on and read a region from. It is a self-contained item: it needs no dialog, area or window around it, only
// its context (below), so the same item sits in the File > Canvas… dialog and in an area of the workspace.
//
// Context: `canvasId` (empty: the newest canvas of the store, or the one a followed node reads), `storeRoot` (empty is
// the session's), and a follow target: `edits` (the document's WidgetEdit) with `followNode`, a Canvas Region node whose
// rectangle is then drawn on the picture, EDITED there (#224) and which **Use for region** writes to.
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
// Simple by default: a canvas, the strip of revisions (Head, candidates, the latest few, Earlier…), the picture
// (wheel zoom, drag to pan, Fit, 100%), and for a candidate Accept, Drop and Export. A candidate shows over the Head;
// Show candidate switches it off to see the Head alone. Compare (against the Head for a candidate, else the parent) opens
// the compare modes of Compare Images (CompareModeButtons.qml). One bar (#228): the canvas and the strip, then the tools
// in chunks (the view, the look, the region, Export, the decision, Drop, Fit and 100%) and the host's `trailing` items
// last (the dialog's close). A run's new candidate shows at once under a banner over the picture's foot (it never moves
// the picture): Back to Head, Accept and Drop; the bar's own Accept and Drop hide meanwhile, so one primary is on screen.
// Keys (docs/ui-ergonomics.md D5, one keymap with the Graph): Home or 0 Fit, 1 100%, + and - zoom, Enter accepts the
// shown candidate (when the focus is in no field and no button), Escape ends a drag, Space held pans. While it has the
// focus the window's bare Q (quit) does not fire here: it is kept for a quick mask (docs/ui-ergonomics.md References);
// Ctrl+Q and the close button still quit, and the Graph's keys sleep while a Canvas area is the active one (Workspace).
// Right to left the bar, the notes and the layers panel mirror with the window; the picture never does.
Item {
    id: root
    objectName: "canvasView"
    required property CanvasLook look
    property alias storeRoot: canvasData.storeRoot
    property alias canvasId: canvasData.canvasId
    property alias edits: canvasData.edits
    property alias followNode: canvasData.followNode
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
    readonly property bool compareOn: comparing && canvasData.hasParent
    readonly property bool canDecide: canvasData.kindB === "candidate" || canvasData.kindB === "dropped"
    readonly property string shownShort: canvasData.revisionB.slice(7, 15)
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
    // Who made a revision, as a person reads it: a Canvas Write of the open document by its title.
    function producerText(producer) {
        if (producer.startsWith("canvas-write:")) {
            const name = canvasData.edits ? canvasData.edits.nodeName(producer.slice(13)) : ""
            return name !== "" ? qsTr("From %1").arg(name) : qsTr("From a Canvas Write")
        }
        return producer.startsWith("canvas-view:") ? qsTr("From an edit in the Canvas view") : producer
    }

    CanvasViewModel {
        id: canvasData
        // A run's new candidate is shown at once (settings.json's canvas_show_new_results, live), unless the view is busy:
        // Compare on, or a rectangle being dragged. Then it waits, and shows when that ends (if still on the Head).
        showNewResults: App.settings ? App.settings.canvasShowNewResults : true
        viewLocked: root.compareOn || root.dragging
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
    FileDialog {
        id: exportDialog
        objectName: "canvasViewExportDialog"
        title: qsTr("Save the revision as a picture")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: [qsTr("PNG images (*.png)")]
        onAccepted: canvasData.exportPicture(selectedFile.toString())
    }
    Component.onDestruction: exportDialog.close()

    // New canvas… and Import picture… (#228, C7), as the canvas picker has them: the canvas is made in this store and shown.
    function newCanvas() {
        (newCanvasAsk.createObject(root) as Question).open()
    }
    function importPicture() {
        (pictureDialog.createObject(root) as FileDialog).open()
    }
    Component {
        id: newCanvasAsk
        NewCanvasQuestion {
            make: (name, width, height, picture) => picture !== "" ? canvasData.importPicture(picture, name)
                                                                  : canvasData.newCanvas(name, width, height)
            sizeOf: url => canvasData.pictureSize(url)
            onClosed: root.makeProblem = ""
        }
    }
    Component {
        id: pictureDialog
        FileDialog {
            id: d
            objectName: "canvasViewImportDialog"
            title: qsTr("Import a picture as a canvas")
            fileMode: FileDialog.OpenFile
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
    function openMenu(button, entries, picked) {
        const at = button.mapToItem(Overlay.overlay, 0, button.height)
        const menu = menuComponent.createObject(root, {wantX: at.x, wantY: at.y, width: Math.max(button.width, 40 * u),
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
        })
    }
    function chooseEarlier() {
        openMenu(earlierButton, canvasData.earlier.map(row => ({text: row.name, value: row.id, detail: root.producerText(row.producer),
                                                                checked: row.id === canvasData.revisionB})),
                 value => canvasData.show(value))
    }
    function readsText(region) {
        return region.reads === "head" || region.reads === "" ? qsTr("Head") : region.readsId !== "" ? region.readsId.slice(7, 15) : region.reads
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
            event.accepted = root.dragging
            return
        }
        const owner = App.commands.ownerOfPress(event.key, event.modifiers)
        event.accepted = (event.key === Qt.Key_Q && bare) || (owner !== "" && App.commands.actsOnGraph(owner))
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape) {
            const frame = regionFrame.item as CropFrame
            if (!frame || !frame.cancel())
                return // not ours: the dialog closes
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

    // The bar: the canvas and the revisions, then the tools. It wraps when the view is narrow.
    Flow {
        id: bar
        objectName: "canvasViewBar"
        width: parent.width
        spacing: root.u
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
        Chunk {
            // The strip: what to look at. The shown chip is the quiet marking: Accept is the one accent fill (D7).
            Repeater {
                model: canvasData.choices
                delegate: ViewButton {
                    required property var modelData
                    objectName: "canvasViewChoice-" + (modelData.kind === "head" ? "head" : modelData.short)
                    look: root.look
                    text: modelData.label
                    tip: root.producerText(modelData.producer)
                    accented: false
                    current: modelData.id === canvasData.revisionB
                    onTapped: canvasData.show(modelData.id)
                }
            }
            ViewButton {
                id: earlierButton
                objectName: "canvasViewEarlier"
                look: root.look
                visible: canvasData.earlier.length > 0
                text: qsTr("Earlier")
                trailingGlyph: "chevron-down"
                tip: qsTr("Revisions before these")
                accented: false
                onTapped: root.chooseEarlier()
            }
        }
    }
    Flow {
        id: tools
        objectName: "canvasViewTools"
        anchors.top: bar.bottom
        anchors.topMargin: root.u
        width: parent.width
        spacing: root.u
        Chunk {
            lead: false
            visible: canvasData.canvasId !== ""
            ViewButton {
                objectName: "canvasViewLayers"
                look: root.look
                glyph: "layers"
                text: qsTr("Layers")
                tip: root.showLayers ? qsTr("Hide the layers panel") : qsTr("Show the layers panel")
                accented: false
                checkable: true
                current: root.showLayers
                onTapped: root.showLayers = !root.showLayers
            }
        }
        // How the shown revision is looked at: over the Head or alone, or against another (the modes).
        Chunk {
            visible: canvasData.canvasId !== ""
            ViewButton {
                objectName: "canvasViewOverlay"
                look: root.look
                visible: canvasData.showsCandidate && !root.compareOn
                text: qsTr("Show candidate")
                tip: qsTr("Show the candidate over the Head; off shows the Head alone")
                accented: false // on is the quiet marking: Accept is the one accent fill (D7)
                checkable: true
                current: canvasData.overlay
                onTapped: canvasData.overlay = !canvasData.overlay
            }
            ViewButton {
                objectName: "canvasViewCompare"
                look: root.look
                enabled: canvasData.hasParent
                text: qsTr("Compare")
                tip: canvasData.hasParent ? (canvasData.showsCandidate ? qsTr("Show this candidate against the Head") : qsTr("Show this revision against its parent"))
                                       : qsTr("The first revision has no parent to compare with")
                accented: false
                checkable: true
                current: root.compareOn
                onTapped: root.comparing = !root.comparing
            }
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
        // The region's own tool, a chunk of its own after the look.
        Chunk {
            visible: root.regionValid
            ViewButton {
                objectName: "canvasViewSnap"
                look: root.look
                enabled: root.regionEditable && canvasData.region.multiple > 1
                text: qsTr("Snap to %1").arg(canvasData.region.multiple)
                tip: qsTr("Put the region's edges on multiples of the node's Multiple of; with a ratio held only the corner moves")
                accented: false
                checkable: true
                current: root.snap
                onTapped: root.snap = !root.snap
            }
        }
        Chunk {
            visible: canvasData.canvasId !== ""
            ViewButton {
                objectName: "canvasViewExport"
                look: root.look
                enabled: canvasData.revisionB !== ""
                text: qsTr("Export…")
                tip: qsTr("Save this revision as a flattened PNG")
                onTapped: {
                    exportDialog.currentFolder = StandardPaths.writableLocation(StandardPaths.PicturesLocation)
                    exportDialog.selectedFile = StandardPaths.writableLocation(StandardPaths.PicturesLocation) + "/" + canvasData.exportName
                    exportDialog.open()
                }
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
                tip: qsTr("Make this candidate the Head of the canvas (Enter)")
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
            visible: children.length > 0
        }
    }

    // The picture, and over it the region of the followed node. Never mirrored: a picture is not text.
    CompareView {
        id: pictureView
        objectName: "canvasViewViewer"
        look: root.look
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        x: root.mirrored ? parent.width - width : 0
        anchors.top: tools.bottom
        anchors.topMargin: root.u
        anchors.bottom: notes.top
        anchors.bottomMargin: root.u
        width: parent.width - (layersPanel.visible ? layersPanel.width + 2 * root.u : 0)
        visible: canvasData.canvasId !== "" && canvasData.revisionB !== ""
        onActiveFocusChanged: if (!activeFocus) root.panKey = false
        mode: "single"
        showZoom: true
        // Alone, the picture is what the overlay says (a candidate over the Head, the Head, a revision); compared, A and B.
        // Each named as CanvasHistory names it.
        sourceA: root.compareOn ? canvasData.sourceA : canvasData.pictureSource
        sourceB: canvasData.sourceB
        sourceDifference: canvasData.sourceDifference
        sizeA: root.compareOn ? canvasData.sizeA : canvasData.pictureSize
        sizeB: canvasData.sizeB
        labelA: root.compareOn ? (root.rowA.name ?? "") : canvasData.showsCandidate && !canvasData.overlay ? qsTr("Head") : (root.rowB.name ?? "")
        labelB: root.rowB.name ?? ""
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
    }
    Item {
        id: regionLayer
        objectName: "canvasViewRegion"
        anchors.fill: pictureView
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        visible: pictureView.visible && canvasData.region.valid === true && !pictureView.paired && pictureView.mode !== "difference"
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
            active: root.regionValid && canvasData.pictureSize.width > 0
            sourceComponent: CropFrame {
                objectName: "canvasViewFrame"
                look: root.look
                imageSize: canvasData.pictureSize
                originX: pictureView.imageX
                originY: pictureView.imageY
                zoom: pictureView.imageScale
                rect: canvasData.region
                dimAlpha: 0
                draw: true
                editable: root.regionEditable
                yieldPress: root.panKey
                constrain: root.snap ? ((r, keepSize) => canvasData.snapRegion(r, keepSize)) : null
                onPressed: pictureView.forceActiveFocus()
                onChanging: r => root.liveRect = r
                onDraggingChanged: if (!dragging) root.liveRect = null
                onCommitted: r => canvasData.setRegion(r)
            }
        }
        // The padded box: the region picker's, dashes on a dark halo (ContextBox.qml).
        ContextBox {
            objectName: "canvasViewContext"
            anchors.fill: parent
            look: root.look
            box: regionLayer.context
        }
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
                text: (canvasData.newResultFrom !== "" ? qsTr("New candidate %1 from %2: it is shown over the Head.").arg(root.shownShort).arg(canvasData.newResultFrom)
                                                         : qsTr("New candidate %1: it is shown over the Head.").arg(root.shownShort))
                      + (canvasData.newResultOthers > 0 ? " " + qsTr("%n more new candidates wait in the strip.", "", canvasData.newResultOthers) : "")
            }
            // Back first, the primary, then Drop in a chunk of its own last (docs/ui-ergonomics.md Order and Destructive).
            Flow {
                width: parent.width
                spacing: root.u
                ViewButton {
                    objectName: "canvasViewNewBack"
                    look: root.look
                    text: qsTr("Back to Head")
                    tip: qsTr("Show the Head; the candidate stays in the strip")
                    onTapped: canvasData.show(canvasData.head)
                }
                Chunk {
                    ViewButton {
                        objectName: "canvasViewNewAccept"
                        look: root.look
                        primary: true
                        text: qsTr("Accept")
                        tip: qsTr("Make this candidate the Head of the canvas (Enter)")
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
            Text {
                objectName: "canvasViewNewResultHint"
                width: parent.width
                wrapMode: Text.Wrap
                color: root.look.label
                font.pixelSize: root.look.captionSize
                text: qsTr("Canvas Write can skip this review: with Keep as candidate off, the Head moves to its result by itself.")
            }
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
        anchors.bottom: parent.bottom
        width: parent.width
        spacing: root.u
        Text {
            objectName: "canvasViewSummary"
            width: parent.width
            wrapMode: Text.Wrap
            visible: text !== ""
            color: root.look.text
            font.pixelSize: root.look.labelSize
            text: canvasData.summary.join("\n")
        }
        // The followed Canvas Region: what it reads, and the one action on it.
        Flow {
            id: regionRow
            objectName: "canvasViewRegionRow"
            width: parent.width
            spacing: 2 * root.u
            visible: canvasData.followNode !== ""
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
                     : root.shownIsHead ? qsTr("Make the region read the Head, whichever revision that is") : qsTr("Make the region read exactly this revision")
                onTapped: canvasData.useForRegion()
            }
        }
        // The rest of what the rectangle means, live while it is dragged: the padded box (dashed) and the model's size.
        Text {
            objectName: "canvasViewRegionInfo"
            width: parent.width
            wrapMode: Text.Wrap
            visible: root.previewReady
            color: root.look.label
            font.pixelSize: root.look.labelSize
            font.features: { "tnum": 1 }
            text: root.previewReady ? qsTr("Context %1 × %2 (dashed). The model gets %3 × %4%5.")
                                       .arg(root.preview.context.width).arg(root.preview.context.height)
                                       .arg(root.preview.modelWidth).arg(root.preview.modelHeight)
                                       .arg(root.preview.capped ? qsTr(", at the %1 MP cap").arg(canvasData.region.cap) : "") : ""
        }
        // How to pick: what to select first, or what a drag does, or why it does nothing.
        Text {
            objectName: "canvasViewRegionHint"
            width: parent.width
            wrapMode: Text.Wrap
            visible: pictureView.visible && (canvasData.followNode === "" || root.regionValid)
            color: root.regionValid && !root.compareOn && !root.regionEditable ? root.look.error : root.look.label
            font.pixelSize: root.look.labelSize
            text: canvasData.followNode === "" ? qsTr("Select a Canvas Region node to pick its region")
                  : root.regionEditable ? qsTr("Drag on the picture to pick the region; Esc cancels. Space-drag or the middle button pans.")
                  : root.compareOn ? qsTr("Turn Compare off to pick the region on the picture.")
                  : qsTr("X, Y, Width or Height is linked to another node: pick it there.")
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
}
