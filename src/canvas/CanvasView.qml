pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Shapes
import Fugue
import "widgets"

// The Canvas view (#221, epics #216 and #217, spec 14.5): one canvas as a picture you can look at, choose a revision of,
// decide on and read a region from. It is a self-contained item: it needs no dialog, area or window around it, only
// its context (below), so the same item sits in the File > Canvas… dialog now and in an area of the workspace later.
//
// Context: `canvasId` (empty: the newest canvas of the store, or the one a followed node reads), `storeRoot` (empty is
// the session's), and a follow target: `edits` (the document's WidgetEdit) with `followNode`, a Canvas Region node whose
// rectangle is then drawn on the picture (read-only here) and which `Region reads from this revision` writes to.
// Everything it does is CanvasViewModel (canvas/canvas_view_model.h) over CanvasHistory.
//
// Simple by default: a canvas, the strip of revisions (Head, candidates, the latest few, Earlier…), the picture
// (wheel zoom, drag to pan, Fit, 100%), and for a candidate Accept, Drop and Export. A candidate shows over the head;
// Show candidate switches it off to see the head alone. Compare (against the head for a candidate, else the parent) opens
// the four compare modes of Compare Images. Keys: 0 Fit, 1 100%, + and - zoom.
Item {
    id: root
    objectName: "canvasView"
    required property CanvasLook look
    property alias storeRoot: canvasData.storeRoot
    property alias canvasId: canvasData.canvasId
    property alias edits: canvasData.edits
    property alias followNode: canvasData.followNode
    // The Fit and 100% buttons of the bar: a host with a bar of its own for them (LargeViewDialog) turns them off.
    property bool zoomButtons: true
    readonly property alias backend: canvasData
    readonly property alias viewer: pictureView
    readonly property real u: look.unit
    property bool comparing: false
    property string compareMode: "slider"
    readonly property bool compareOn: comparing && canvasData.hasParent
    readonly property bool canDecide: canvasData.kindB === "candidate" || canvasData.kindB === "dropped"
    readonly property string shownShort: canvasData.revisionB.slice(7, 15)
    readonly property bool shownIsHead: canvasData.revisionB !== "" && canvasData.revisionB === canvasData.head

    function fit() { pictureView.fit() }
    function actualSize() { pictureView.actualSize() }
    // Reads the store again now (a host shows it again after it was away).
    function refresh() { canvasData.refresh() }

    CanvasViewModel { id: canvasData }

    Component.onCompleted: {
        // One canvas by default: with none chosen, the newest.
        if (canvasData.canvasId === "" && canvasData.canvases.length > 0)
            canvasData.canvasId = canvasData.canvases[0].id
    }
    onCompareOnChanged: pictureView.setView({mode: compareOn ? compareMode : "single"})

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
        const menu = menuComponent.createObject(root, {wantX: at.x, wantY: at.y, width: Math.max(button.width, 24 * u),
                                                       anchorHeight: button.height, entries: entries, searchable: entries.length > 12}) as WidgetMenu
        menu.picked.connect(picked)
        menu.open()
    }
    function chooseCanvas() {
        const rows = canvasData.canvases
        openMenu(canvasButton, rows.map(row => ({text: row.name, value: row.id, detail: row.size, checked: row.id === canvasData.canvasId})),
                 value => canvasData.canvasId = value)
    }
    function chooseEarlier() {
        openMenu(earlierButton, canvasData.earlier.map(row => ({text: row.short, value: row.id, detail: row.producer, checked: row.id === canvasData.revisionB})),
                 value => canvasData.show(value))
    }
    function readsText(region) {
        return region.reads === "head" || region.reads === "" ? qsTr("head") : region.readsId !== "" ? region.readsId.slice(7, 15) : region.reads
    }

    Keys.onPressed: event => {
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier))
            return
        if (event.key === Qt.Key_0) fit()
        else if (event.key === Qt.Key_1) actualSize()
        else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) pictureView.zoomAt(pictureView.width / 2, pictureView.height / 2, 1.25)
        else if (event.key === Qt.Key_Minus) pictureView.zoomAt(pictureView.width / 2, pictureView.height / 2, 0.8)
        else return
        event.accepted = true
    }

    // The bar: the canvas, the revisions, then the tools. It wraps when the view is narrow.
    Flow {
        id: bar
        objectName: "canvasViewBar"
        width: parent.width
        spacing: root.u
        ViewButton {
            id: canvasButton
            objectName: "canvasViewCanvas"
            look: root.look
            text: (canvasData.canvases.find(row => row.id === canvasData.canvasId)?.name ?? qsTr("Canvas")) + " ▾"
            tip: qsTr("Choose the canvas to look at")
            accented: false
            onTapped: root.chooseCanvas()
        }
        Item { width: root.u; height: 1 }
        Repeater {
            model: canvasData.choices
            delegate: ViewButton {
                id: chip
                required property var modelData
                objectName: "canvasViewChoice-" + (modelData.kind === "head" ? "head" : modelData.short)
                look: root.look
                text: modelData.kind === "candidate" ? qsTr("Candidate %1").arg(modelData.short) : modelData.label
                tip: modelData.producer
                current: modelData.id === canvasData.revisionB
                onTapped: canvasData.show(modelData.id)
            }
        }
        ViewButton {
            id: earlierButton
            objectName: "canvasViewEarlier"
            look: root.look
            visible: canvasData.earlier.length > 0
            text: qsTr("Earlier") + " ▾"
            tip: qsTr("Revisions before these")
            accented: false
            onTapped: root.chooseEarlier()
        }
    }
    Flow {
        id: tools
        objectName: "canvasViewTools"
        anchors.top: bar.bottom
        anchors.topMargin: root.u
        width: parent.width
        spacing: root.u
        ViewButton {
            objectName: "canvasViewFit"
            look: root.look
            visible: root.zoomButtons
            text: qsTr("Fit")
            accented: false
            current: !pictureView.actual && pictureView.zoom <= 1
            onTapped: root.fit()
        }
        ViewButton {
            objectName: "canvasViewActual"
            look: root.look
            visible: root.zoomButtons
            text: qsTr("100%")
            accented: false
            current: pictureView.actual
            onTapped: root.actualSize()
        }
        Item { width: root.u; height: 1; visible: root.zoomButtons }
        ViewButton {
            objectName: "canvasViewOverlay"
            look: root.look
            visible: canvasData.showsCandidate && !root.compareOn
            text: qsTr("Show candidate")
            tip: qsTr("Show the candidate over the head; off shows the head alone")
            current: canvasData.overlay
            onTapped: canvasData.overlay = !canvasData.overlay
        }
        ViewButton {
            objectName: "canvasViewCompare"
            look: root.look
            visible: canvasData.canvasId !== ""
            enabled: canvasData.hasParent
            text: qsTr("Compare")
            tip: canvasData.hasParent ? (canvasData.showsCandidate ? qsTr("Show this candidate against the head") : qsTr("Show this revision against its parent"))
                                   : qsTr("The first revision has no parent to compare with")
            current: root.compareOn
            onTapped: root.comparing = !root.comparing
        }
        Repeater {
            model: root.compareOn ? [{id: "slider", text: qsTr("Slider")}, {id: "side_by_side", text: qsTr("Side by side")},
                                     {id: "flip", text: qsTr("Flip")}, {id: "difference", text: qsTr("Difference")}] : []
            delegate: ViewButton {
                required property var modelData
                objectName: "canvasViewMode-" + modelData.id
                look: root.look
                text: modelData.text
                current: pictureView.mode === modelData.id
                enabled: modelData.id !== "difference" || canvasData.sourceDifference.toString() !== ""
                onTapped: {
                    root.compareMode = modelData.id
                    pictureView.setView({mode: modelData.id})
                }
            }
        }
        Item { width: 2 * root.u; height: 1 }
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
        Item { width: 2 * root.u; height: 1; visible: root.canDecide }
        ViewButton {
            objectName: "canvasViewAccept"
            look: root.look
            visible: root.canDecide
            primary: true
            text: qsTr("Accept")
            tip: qsTr("Make this candidate the head of the canvas")
            onTapped: canvasData.accept(canvasData.revisionB)
        }
        Item { width: 4 * root.u; height: 1; visible: canvasData.kindB === "candidate" } // the destructive chunk: twice the gap inside the others
        ViewButton {
            objectName: "canvasViewDrop"
            look: root.look
            visible: canvasData.kindB === "candidate"
            danger: true
            text: qsTr("Drop")
            tip: qsTr("Hide this candidate (it is kept, and Accept still takes it)")
            onTapped: canvasData.drop(canvasData.revisionB)
        }
    }

    // The picture, and over it the region of the followed node.
    CompareView {
        id: pictureView
        objectName: "canvasViewViewer"
        look: root.look
        anchors.top: tools.bottom
        anchors.topMargin: root.u
        anchors.bottom: notes.top
        anchors.bottomMargin: root.u
        width: parent.width
        visible: canvasData.canvasId !== "" && canvasData.revisionB !== ""
        mode: "single"
        showZoom: true
        // Alone, the picture is what the overlay says (a candidate over the head, the head, a revision); compared, A and B.
        sourceA: root.compareOn ? canvasData.sourceA : canvasData.pictureSource
        sourceB: canvasData.sourceB
        sourceDifference: canvasData.sourceDifference
        sizeA: root.compareOn ? canvasData.sizeA : canvasData.pictureSize
        sizeB: canvasData.sizeB
        labelA: root.compareOn ? qsTr("A") : canvasData.showsCandidate && canvasData.overlay ? qsTr("Candidate %1").arg(root.shownShort)
                                           : root.shownIsHead || canvasData.showsCandidate ? qsTr("Head") : root.shownShort
        labelB: canvasData.showsCandidate ? qsTr("Candidate %1").arg(root.shownShort) : qsTr("B")
    }
    Item {
        id: regionLayer
        objectName: "canvasViewRegion"
        anchors.fill: pictureView
        visible: pictureView.visible && canvasData.region.valid === true && !pictureView.paired && pictureView.mode !== "difference"
        clip: true
        readonly property real k: pictureView.imageScale
        readonly property var box: canvasData.region.valid ? canvasData.region : ({x: 0, y: 0, width: 0, height: 0, context: {x: 0, y: 0, width: 0, height: 0}})
        // The rectangle and its padded context, in the picture's coordinates, which move with zoom and pan.
        readonly property rect rectangle: Qt.rect(pictureView.imageX + box.x * k, pictureView.imageY + box.y * k, box.width * k, box.height * k)
        readonly property rect context: Qt.rect(pictureView.imageX + box.context.x * k, pictureView.imageY + box.context.y * k, box.context.width * k, box.context.height * k)
        Shape {
            anchors.fill: parent
            ShapePath {
                strokeColor: Qt.rgba(0, 0, 0, 0.6)
                strokeWidth: 3.5
                fillColor: "transparent"
                startX: regionLayer.rectangle.left; startY: regionLayer.rectangle.top
                PathLine { x: regionLayer.rectangle.right; y: regionLayer.rectangle.top }
                PathLine { x: regionLayer.rectangle.right; y: regionLayer.rectangle.bottom }
                PathLine { x: regionLayer.rectangle.left; y: regionLayer.rectangle.bottom }
                PathLine { x: regionLayer.rectangle.left; y: regionLayer.rectangle.top }
            }
            ShapePath {
                strokeColor: root.look.accent
                strokeWidth: 2
                fillColor: "transparent"
                startX: regionLayer.rectangle.left; startY: regionLayer.rectangle.top
                PathLine { x: regionLayer.rectangle.right; y: regionLayer.rectangle.top }
                PathLine { x: regionLayer.rectangle.right; y: regionLayer.rectangle.bottom }
                PathLine { x: regionLayer.rectangle.left; y: regionLayer.rectangle.bottom }
                PathLine { x: regionLayer.rectangle.left; y: regionLayer.rectangle.top }
            }
            ShapePath {
                strokeColor: root.look.accent
                strokeWidth: 1.5
                strokeStyle: ShapePath.DashLine
                dashPattern: [4, 3]
                fillColor: "transparent"
                startX: regionLayer.context.left; startY: regionLayer.context.top
                PathLine { x: regionLayer.context.right; y: regionLayer.context.top }
                PathLine { x: regionLayer.context.right; y: regionLayer.context.bottom }
                PathLine { x: regionLayer.context.left; y: regionLayer.context.bottom }
                PathLine { x: regionLayer.context.left; y: regionLayer.context.top }
            }
        }
    }
    // What the picture area says when there is nothing to show.
    Text {
        objectName: "canvasViewEmpty"
        anchors.fill: pictureView
        visible: !pictureView.visible
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
        color: root.look.label
        font.pixelSize: root.look.labelSize
        text: canvasData.canvasId === "" ? (canvasData.canvases.length === 0 ? qsTr("No canvas yet: fugue canvas new, or fugue canvas import, makes one.")
                                                                       : qsTr("Choose a canvas"))
                                      : canvasData.error !== "" ? canvasData.error : qsTr("This canvas has no revision yet.")
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
                height: useButton.height
                verticalAlignment: Text.AlignVCenter
                color: canvasData.region.valid ? root.look.label : root.look.error
                font.pixelSize: root.look.labelSize
                text: canvasData.region.valid ? qsTr("Region %1 reads %2: %3 × %4 at %5, %6").arg(canvasData.followNode).arg(root.readsText(canvasData.region))
                                                  .arg(canvasData.region.width).arg(canvasData.region.height).arg(canvasData.region.x).arg(canvasData.region.y)
                                           : canvasData.region.reason
            }
            ViewButton {
                id: useButton
                objectName: "canvasViewUseRegion"
                look: root.look
                visible: canvasData.region.valid === true
                enabled: canvasData.region.writable === true && canvasData.region.readsShown !== true && canvasData.revisionB !== ""
                text: qsTr("Region reads from this revision")
                tip: canvasData.region.writable !== true ? qsTr("The Revision input is linked to another node: change it there")
                     : canvasData.region.readsShown === true ? qsTr("The region reads this revision already")
                     : root.shownIsHead ? qsTr("Make the region read the head, whichever revision that is") : qsTr("Make the region read exactly this revision")
                onTapped: canvasData.useForRegion()
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
            visible: canvasData.error !== "" && pictureView.visible
            color: root.look.error
            font.pixelSize: root.look.labelSize
            text: canvasData.error
        }
    }
}
