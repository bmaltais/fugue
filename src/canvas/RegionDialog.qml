pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import Fugue

// The region picker of a Canvas Region node (#213, epic #206, spec 14.5): the canvas at the node's
// revision (head, named now) in the Crop window's viewer, zoom and pan as everywhere, and a rectangle to drag
// out or resize by its handles. X, Y, Width and Height stay as numbers under it, in step with the rectangle.
// The padded area the node will cut is the dashed outer box, and under the picture a line says the size the
// model will get. A ratio lock (as the Crop window's) and Snap (edges on the node's Multiple of) are in the bar.
// OK (or Enter) writes x, y, width and height to the node as ONE undo step; Cancel, the close button and Escape leave
// the node as it was. `openFor(node)` opens it; the work is RegionPicker (region_picker.h) and the numbers
// canvas/region_pick.h, which call the node's own functions, so the box and size are what the node computes.
LargeViewDialog {
    id: root
    objectName: "regionDialog"
    name: "region"
    look: App.theme.look
    required property WidgetEdit edits
    property string nodeId
    property var info: ({})
    // The rectangle being edited (canvas pixels): the committed one, and the one a drag shows now.
    property var draft: ({x: 0, y: 0, width: 1, height: 1})
    property var live: draft
    property string ratioName: "free"
    property bool snap: false
    readonly property real ratio: CropGeometry.ratioOf(ratioName, backend.canvasSize)
    readonly property bool ready: backend.error === "" && backend.canvasSize.width > 0
    readonly property alias picker: backend
    readonly property alias cropView: picture
    readonly property var context: ready ? backend.context(live) : ({x: 0, y: 0, width: 0, height: 0})
    readonly property var modelSize: ready ? backend.modelSize(live) : ({width: 0, height: 0, capped: false})
    zoomable: ready ? picture.viewer : null

    RegionPicker { id: backend }

    function openFor(node) {
        nodeId = node
        info = edits.regionState(node)
        backend.storeRoot = info.storeRoot
        backend.canvasId = info.canvas
        backend.revision = info.revision
        backend.padding = info.padding
        backend.scale = info.scale
        backend.cap = info.cap
        backend.multiple = info.multiple
        ratioName = "free"
        snap = false
        if (backend.load()) {
            picture.source = backend.source
            picture.pixels = backend.canvasSize
            draft = backend.initial(info.x, info.y, info.width, info.height)
            live = draft
            picture.viewer.fit()
        }
        open()
    }
    // OK: the four numbers go to the node, one undo step.
    function apply() {
        if (!ready || !info.editable)
            return
        edits.setRegion(nodeId, draft.x, draft.y, draft.width, draft.height)
        close()
    }
    function settle(rect) {
        // The edges on the multiple when Snap is on; with a ratio lock the size is left as it is.
        const r = snap ? backend.snapped(rect, ratio > 0) : rect
        draft = r
        live = r
    }
    function pickRatio(name) {
        ratioName = name
        settle(CropGeometry.fitRatio(draft, ratio, backend.canvasSize))
    }
    function turnedRatio() {
        const m = /^(\d+(?:\.\d+)?):(\d+(?:\.\d+)?)$/.exec(ratioName)
        return m ? m[2] + ":" + m[1] : ""
    }
    // A number typed in a field: x and y move the rectangle, width and height resize it (keeping the lock).
    function setField(field, text) {
        const v = parseInt(text)
        if (isNaN(v))
            return
        const size = backend.canvasSize
        let r
        if (field === "x") r = CropGeometry.moved(draft, v - draft.x, 0, size)
        else if (field === "y") r = CropGeometry.moved(draft, 0, v - draft.y, size)
        else if (field === "width") r = CropGeometry.resized(draft, "right", v - draft.width, 0, size, ratio, false)
        else r = CropGeometry.resized(draft, "bottom", 0, v - draft.height, size, ratio, false)
        settle(r)
    }
    onConfirm: apply()

    tools: Row {
        spacing: root.look.unit
        Repeater {
            model: [{id: "free", text: qsTr("Free")}, {id: "source", text: qsTr("Original")}, {id: "1:1", text: "1:1"},
                    {id: "4:3", text: "4:3"}, {id: "3:2", text: "3:2"}, {id: "16:9", text: "16:9"}]
            delegate: ViewButton {
                required property var modelData
                objectName: "regionRatio-" + modelData.id
                look: root.look
                text: modelData.text
                accented: false // the chosen ratio is the quiet marking: OK is the one accent fill (D7)
                current: root.ratioName === modelData.id
                enabled: root.ready
                onTapped: root.pickRatio(modelData.id)
            }
        }
        ViewButton {
            objectName: "regionRatioTurn"
            look: root.look
            text: qsTr("Turn")
            enabled: root.ready && root.turnedRatio() !== ""
            onTapped: root.pickRatio(root.turnedRatio())
        }
        Item { width: 2 * root.look.unit; height: 1 }
        ViewButton {
            objectName: "regionSnap"
            look: root.look
            text: qsTr("Snap to %1").arg(backend.multiple)
            tip: qsTr("Put the edges on multiples of the node's Multiple of; with a ratio lock only the corner moves")
            accented: false // on is the quiet marking: OK is the one accent fill (docs/ui-ergonomics.md, D7)
            checkable: true
            current: root.snap
            enabled: root.ready && backend.multiple > 1
            onTapped: {
                root.snap = !root.snap
                if (root.snap)
                    root.settle(root.draft)
            }
        }
    }

    Item {
        anchors.fill: parent
        CropView {
            id: picture
            objectName: "regionView"
            anchors.fill: parent
            visible: root.ready
            look: root.look
            rect: root.ready ? root.draft : null
            ratio: root.ratio
            onCommitted: r => root.settle(r)
            onChanging: r => root.live = root.snap ? root.picker.snapped(r, root.ratio > 0) : r
        }
        // The padded area the node cuts: a dashed outer box over the picture, never taking the pointer.
        Shape {
            id: outer
            objectName: "regionContext"
            anchors.fill: parent
            visible: root.ready
            clip: true
            readonly property real k: picture.viewer.imageScale
            readonly property real boxLeft: picture.viewer.imageX + root.context.x * k
            readonly property real boxTop: picture.viewer.imageY + root.context.y * k
            readonly property real boxRight: boxLeft + root.context.width * k
            readonly property real boxBottom: boxTop + root.context.height * k
            ShapePath {
                strokeColor: Qt.rgba(0, 0, 0, 0.6)
                strokeWidth: 3
                fillColor: "transparent"
                startX: outer.boxLeft; startY: outer.boxTop
                PathLine { x: outer.boxRight; y: outer.boxTop }
                PathLine { x: outer.boxRight; y: outer.boxBottom }
                PathLine { x: outer.boxLeft; y: outer.boxBottom }
                PathLine { x: outer.boxLeft; y: outer.boxTop }
            }
            ShapePath {
                strokeColor: root.look.accent
                strokeWidth: 1.5
                strokeStyle: ShapePath.DashLine
                dashPattern: [4, 3]
                fillColor: "transparent"
                startX: outer.boxLeft; startY: outer.boxTop
                PathLine { x: outer.boxRight; y: outer.boxTop }
                PathLine { x: outer.boxRight; y: outer.boxBottom }
                PathLine { x: outer.boxLeft; y: outer.boxBottom }
                PathLine { x: outer.boxLeft; y: outer.boxTop }
            }
        }
        Text {
            objectName: "regionError"
            visible: !root.ready
            anchors.centerIn: parent
            width: parent.width * 0.7
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: backend.error !== "" ? backend.error : qsTr("Choose a canvas first: there is no picture to pick a region on.")
            color: root.look.error
            font.pixelSize: root.look.labelSize
        }
    }

    footer: Column {
        spacing: root.look.unit
        Text {
            objectName: "regionInfo"
            width: parent.width
            elide: Text.ElideRight
            visible: root.ready
            text: qsTr("%1 × %2 canvas at %3 (%4). Context %5 × %6 (dashed). The model gets %7 × %8%9.")
                  .arg(backend.canvasSize.width).arg(backend.canvasSize.height)
                  .arg(root.info.revision === "head" ? qsTr("head") : qsTr("revision"))
                  .arg(backend.shortRevision).arg(root.context.width).arg(root.context.height)
                  .arg(root.modelSize.width).arg(root.modelSize.height)
                  .arg(root.modelSize.capped ? qsTr(", at the %1 MP cap").arg(root.info.cap) : "")
            color: root.look.label
            font.pixelSize: root.look.labelSize
            font.features: { "tnum": 1 }
        }
        Text {
            objectName: "regionNote"
            width: parent.width
            elide: Text.ElideRight
            visible: text !== ""
            text: !root.info.editable ? qsTr("X, Y, Width or Height is linked to another node: pick it there.")
                  : root.info.maskLinked ? qsTr("A mask is connected, so the node uses the mask's bounds and not this rectangle.") : ""
            color: root.look.error
            font.pixelSize: root.look.labelSize
        }
        Row {
            spacing: 2 * root.look.unit
            Repeater {
                model: [{id: "x", text: qsTr("X")}, {id: "y", text: qsTr("Y")}, {id: "width", text: qsTr("Width")}, {id: "height", text: qsTr("Height")}]
                delegate: Row {
                    id: entry
                    required property var modelData
                    spacing: root.look.unit
                    height: field.implicitHeight
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.modelData.text
                        color: root.look.label
                        font.pixelSize: root.look.labelSize
                    }
                    ThemedTextField {
                        id: field
                        objectName: "regionField-" + entry.modelData.id
                        width: 14 * root.look.unit
                        enabled: root.ready
                        text: String(root.live[entry.modelData.id])
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { bottom: 0; top: 99999 }
                        selectByMouse: true
                        // Typing edits the text; the rectangle follows when it is entered or left.
                        onEditingFinished: {
                            root.setField(entry.modelData.id, text)
                            text = String(root.draft[entry.modelData.id])
                        }
                        Connections {
                            target: root
                            function onLiveChanged() { if (!field.activeFocus) field.text = String(root.live[entry.modelData.id]) }
                        }
                    }
                }
            }
            Item { width: 2 * root.look.unit; height: 1 }
            ViewButton {
                objectName: "regionCancel"
                look: root.look
                text: qsTr("Cancel")
                onTapped: root.close()
            }
            ViewButton {
                objectName: "regionApply"
                look: root.look
                text: qsTr("OK") // as the Crop window's and the mask editor's: it writes and closes
                primary: true
                enabled: root.ready && root.info.editable === true
                onTapped: root.apply()
            }
        }
    }
}
