pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Dialogs
import Fugue

// The revisions of a canvas (#210, #214, epic #206, spec 14.5) in a large view: the list of what the canvas
// went through and the candidates waiting for review on the left, and the picture of the selected revision on
// the right at full size (zoom and pan), the head at first. Compare puts it against its parent (or any other
// revision) in the same compareViewer as Compare Images; Export saves the shown revision as a PNG. A candidate
// is accepted into the head or dropped from the bar. Without a canvas it lists the
// canvases of the store to open one. `openFor(id)` opens it on a canvas (empty: the list); Escape closes.
//
// Click a revision: show it (Compare on: against its parent). Ctrl+click: make it the A side of a comparison,
// which turns Compare on.
// Up and Down move the selection. The work is CanvasHistory (canvas/canvas_history.h).
LargeViewDialog {
    id: root
    objectName: "canvasHistoryDialog"
    name: "canvasHistory"
    look: App.theme.look
    readonly property alias history: backend
    readonly property alias viewer: compareViewer
    // A test points the dialog at a store of its own.
    property alias storeRoot: backend.storeRoot
    zoomable: backend.canvasId !== "" ? compareViewer : null
    readonly property real u: look.unit
    // Compare is a toggle: off shows the revision alone (CompareView's single mode), on the compare modes.
    property bool comparing: false
    property string compareMode: "slider"
    readonly property bool compareOn: comparing && backend.hasParent
    readonly property bool canDecide: backend.kindB === "candidate" || backend.kindB === "dropped"
    // The canvas the keys are on in the list of canvases (Up, Down, Enter), as the revision rows have.
    property int canvasIndex: 0

    function openFor(canvasId) {
        backend.canvasId = canvasId
        backend.reload()
        open()
    }

    CanvasHistory { id: backend }

    onCompareOnChanged: compareViewer.setView({mode: compareOn ? compareMode : "single"})

    // The picture is saved by the file dialog, as Save Image does: the backend writes it and never replaces a file.
    FileDialog {
        id: exportDialog
        objectName: "canvasHistoryExportDialog"
        title: qsTr("Save the revision as a picture")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: [qsTr("PNG images (*.png)")]
        onAccepted: backend.exportPicture(selectedFile.toString())
    }
    Component.onDestruction: exportDialog.close()

    onKeyPressed: event => {
        if (backend.canvasId === "") {
            // No viewer yet, so the dialog passes Enter on too (LargeViewDialog's `confirm` needs a `zoomable`).
            const rows = backend.canvases
            if (event.key === Qt.Key_Down && canvasIndex < rows.length - 1) canvasIndex++
            else if (event.key === Qt.Key_Up && canvasIndex > 0) canvasIndex--
            else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && canvasIndex < rows.length) backend.canvasId = rows[canvasIndex].id
            else return
            event.accepted = true
            return
        }
        const rows = backend.revisions
        const at = rows.findIndex(row => row.id === backend.revisionB)
        if (event.key === Qt.Key_Down && at < rows.length - 1) backend.select(rows[at + 1].id)
        else if (event.key === Qt.Key_Up && at > 0) backend.select(rows[at - 1].id)
        else return
        event.accepted = true
    }

    tools: Row {
        spacing: root.look.unit
        ViewButton {
            objectName: "canvasHistoryCanvases"
            look: root.look
            visible: backend.canvasId !== ""
            glyph: "chevron-left"
            text: qsTr("Canvases")
            accented: false
            onTapped: backend.canvasId = ""
        }
        Item { width: 2 * root.u; height: 1; visible: backend.canvasId !== "" }
        ViewButton {
            objectName: "canvasHistoryCompare"
            look: root.look
            visible: backend.canvasId !== ""
            enabled: backend.hasParent
            text: qsTr("Compare")
            tip: !backend.hasParent ? qsTr("The first revision has no parent to compare with")
                 : root.canDecide ? qsTr("Show this candidate against the head") : qsTr("Show this revision against its parent")
            // On is the quiet marking: Accept is the one accent fill of this window (docs/ui-ergonomics.md, D7).
            accented: false
            checkable: true
            current: root.compareOn
            onTapped: root.comparing = !root.comparing
        }
        Repeater {
            model: backend.canvasId === "" || !root.compareOn ? [] : [{id: "slider", text: qsTr("Slider")}, {id: "side_by_side", text: qsTr("Side by side")},
                                                   {id: "flip", text: qsTr("Flip")}, {id: "difference", text: qsTr("Difference")}]
            delegate: ViewButton {
                required property var modelData
                objectName: "canvasHistoryMode-" + modelData.id
                look: root.look
                text: modelData.text
                accented: false
                current: compareViewer.mode === modelData.id
                enabled: modelData.id !== "difference" || backend.sourceDifference.toString() !== ""
                onTapped: {
                    root.compareMode = modelData.id
                    compareViewer.setView({mode: modelData.id})
                }
            }
        }
        Item { width: 2 * root.u; height: 1; visible: backend.canvasId !== "" }
        ViewButton {
            objectName: "canvasHistoryExport"
            look: root.look
            visible: backend.canvasId !== ""
            enabled: backend.revisionB !== ""
            text: qsTr("Export…")
            tip: qsTr("Save this revision as a flattened PNG")
            onTapped: {
                exportDialog.currentFolder = StandardPaths.writableLocation(StandardPaths.PicturesLocation)
                exportDialog.selectedFile = StandardPaths.writableLocation(StandardPaths.PicturesLocation) + "/" + backend.exportName
                exportDialog.open()
            }
        }
        Item { width: 2 * root.u; height: 1; visible: root.canDecide }
        ViewButton {
            objectName: "canvasHistoryAccept"
            look: root.look
            visible: root.canDecide
            primary: true
            text: qsTr("Accept")
            tip: qsTr("Make this candidate the head of the canvas")
            onTapped: backend.accept(backend.revisionB)
        }
        Item { width: 4 * root.u; height: 1; visible: backend.kindB === "candidate" } // the destructive chunk: twice the gap inside the others
        ViewButton {
            objectName: "canvasHistoryDrop"
            look: root.look
            visible: backend.kindB === "candidate"
            danger: true
            text: qsTr("Drop")
            tip: qsTr("Hide this candidate (it is kept, and Accept still takes it)")
            onTapped: backend.drop(backend.revisionB)
        }
    }

    // The canvases of the store, when none is chosen.
    ListView {
        id: canvasList
        objectName: "canvasHistoryCanvasList"
        anchors.fill: parent
        visible: backend.canvasId === ""
        clip: true
        model: backend.canvases
        header: Text {
            width: canvasList.width
            height: 8 * root.u
            verticalAlignment: Text.AlignVCenter
            color: root.look.label
            font.pixelSize: root.look.labelSize
            text: canvasList.count === 0 ? qsTr("No canvas yet: fugue canvas new, or fugue canvas import, makes one.") : qsTr("Choose a canvas")
        }
        delegate: Rectangle {
            id: canvasRow
            required property var modelData
            required property int index
            objectName: "canvasHistoryCanvas-" + modelData.id
            width: canvasList.width
            height: 8 * root.u
            readonly property bool isCurrent: index === root.canvasIndex
            color: tap.pressed ? root.look.selectedRow : hover.hovered || isCurrent ? root.look.menuRow : "transparent"
            Row {
                anchors.verticalCenter: parent.verticalCenter
                x: 2 * root.u
                spacing: 3 * root.u
                Text { text: canvasRow.modelData.name; color: root.look.text; font.pixelSize: root.look.textSize; font.weight: Font.DemiBold }
                Text { text: canvasRow.modelData.id; color: root.look.label; font.pixelSize: root.look.labelSize; anchors.baseline: parent.children[0].baseline }
            }
            HoverHandler { id: hover }
            TapHandler { id: tap; onTapped: backend.canvasId = canvasRow.modelData.id }
            Accessible.role: Accessible.Button
            Accessible.name: modelData.name
        }
    }

    // The revisions and the comparison, when one is.
    Row {
        anchors.fill: parent
        spacing: 2 * root.u
        visible: backend.canvasId !== ""

        ListView {
            id: revisionList
            objectName: "canvasHistoryRevisions"
            width: 34 * root.u
            height: parent.height
            clip: true
            model: backend.revisions
            header: Text {
                width: revisionList.width
                height: 7 * root.u
                verticalAlignment: Text.AlignVCenter
                color: root.look.label
                font.pixelSize: root.look.labelSize
                text: qsTr("Revisions, newest first")
            }
            delegate: Rectangle {
                id: row
                required property var modelData
                objectName: "canvasHistoryRevision-" + modelData.short
                width: revisionList.width
                height: 8 * root.u // above the 24px floor
                readonly property bool isB: modelData.id === backend.revisionB
                readonly property bool isA: modelData.id === backend.revisionA && !isB
                color: isB ? root.look.selectedRow : rowTap.pressed || rowHover.hovered ? root.look.menuRow : "transparent"
                radius: root.look.smallCorner
                border.width: isA ? 1 : 0
                border.color: root.look.accent
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    x: 2 * root.u
                    Text {
                        text: row.modelData.short + (row.isB ? "  B" : row.isA ? "  A" : "")
                        color: root.look.text
                        font.pixelSize: root.look.textSize
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: row.modelData.producer
                        color: root.look.label
                        font.pixelSize: root.look.captionSize
                        elide: Text.ElideRight
                        width: revisionList.width - 16 * root.u
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    anchors.rightMargin: 2 * root.u
                    visible: row.modelData.kind !== "backend"
                    text: row.modelData.kind === "head" ? qsTr("Head") : row.modelData.kind === "dropped" ? qsTr("Dropped") : qsTr("Candidate")
                    color: row.modelData.kind === "candidate" ? root.look.accent : root.look.label
                    font.pixelSize: root.look.captionSize
                    font.weight: Font.DemiBold
                }
                HoverHandler { id: rowHover }
                TapHandler {
                    id: rowTap
                    acceptedModifiers: Qt.NoModifier
                    onTapped: backend.select(row.modelData.id)
                }
                TapHandler {
                    acceptedModifiers: Qt.ControlModifier
                    onTapped: {
                        backend.setA(row.modelData.id)
                        root.comparing = true
                    }
                }
                Accessible.role: Accessible.ListItem
                Accessible.name: row.modelData.short + ", " + row.modelData.producer + ", " + row.modelData.kind
            }
        }

        Column {
            width: parent.width - revisionList.width - parent.spacing
            height: parent.height
            spacing: root.u
            CompareView {
                id: compareViewer
                objectName: "canvasHistoryViewer"
                look: root.look
                width: parent.width
                height: parent.height - notes.height - parent.spacing
                mode: "single"
                showZoom: true
                // Alone, the picture is B (the selected revision) in A's slot; compared, A and B are the two sides.
                sourceA: root.compareOn ? backend.sourceA : backend.sourceB
                sourceB: backend.sourceB
                sourceDifference: backend.sourceDifference
                sizeA: root.compareOn ? backend.sizeA : backend.sizeB
                sizeB: backend.sizeB
                labelA: root.compareOn ? qsTr("A") : qsTr("B")
                labelB: qsTr("B")
            }
            Column {
                id: notes
                width: parent.width
                Text {
                    objectName: "canvasHistorySummary"
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: root.look.text
                    font.pixelSize: root.look.labelSize
                    text: backend.summary.join("\n")
                }
                Text {
                    objectName: "canvasHistoryNotice"
                    width: parent.width
                    wrapMode: Text.Wrap
                    visible: backend.notice !== ""
                    color: root.look.label
                    font.pixelSize: root.look.labelSize
                    text: backend.notice
                }
                Text {
                    objectName: "canvasHistoryError"
                    width: parent.width
                    wrapMode: Text.Wrap
                    visible: backend.error !== ""
                    color: root.look.error
                    font.pixelSize: root.look.labelSize
                    text: backend.error
                }
            }
        }
    }
}
