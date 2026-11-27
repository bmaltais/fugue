pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../kit"

// The larger crop window of a Crop node (#73): the picture at full size with the same frame and
// handles, the four numbers editable, an aspect lock (free, the source's, 1:1, 4:3, 3:2, 16:9, and
// the turn that makes 16:9 into 9:16), Shift for the ratio and Alt for from the center while dragging
// (Photoshop's). OK or Enter writes the four numbers to the node, one undo step; Cancel, the close
// button or Escape leaves the node as it was. `openFor(node)` opens it.
LargeViewDialog {
    id: root
    objectName: "cropDialog"
    name: "crop"
    look: App.theme.look
    required property CanvasModel canvasModel
    property string nodeId
    // The frame being edited (pixels) and the lock's name ("free", "source" or "w:h").
    property var draft: ({x: 0, y: 0, width: 1, height: 1})
    property string ratioName: "free"
    readonly property real ratio: CropGeometry.ratioOf(ratioName, picture.pixels)
    readonly property alias cropView: picture
    zoomable: picture.viewer

    function openFor(node) {
        nodeId = node
        picture.source = App.runs ? App.runs.imageUrl(node) : ""
        picture.pixels = canvasModel.previewSize(node)
        draft = CropGeometry.applied(canvasModel.cropRect(node), picture.pixels)
        ratioName = "free"
        picture.viewer.fit()
        open()
    }
    // OK: the numbers go to the node, one undo step.
    function apply() {
        canvasModel.setCropRect(nodeId, draft)
        close()
    }
    function pickRatio(name) {
        ratioName = name
        draft = CropGeometry.fitRatio(draft, ratio, picture.pixels)
    }
    // "16:9" turned: "9:16". The source's and the free ratio have no sides to swap.
    function turnedRatio() {
        const m = /^(\d+(?:\.\d+)?):(\d+(?:\.\d+)?)$/.exec(ratioName)
        return m ? m[2] + ":" + m[1] : ""
    }
    // A number typed in a field: x and y move the frame, width and height resize it (keeping the lock).
    function setField(field, text) {
        const v = parseInt(text)
        if (isNaN(v))
            return
        const size = picture.pixels
        if (field === "x") draft = CropGeometry.moved(draft, v - draft.x, 0, size)
        else if (field === "y") draft = CropGeometry.moved(draft, 0, v - draft.y, size)
        else if (field === "width") draft = CropGeometry.resized(draft, "right", v - draft.width, 0, size, ratio, false)
        else draft = CropGeometry.resized(draft, "bottom", 0, v - draft.height, size, ratio, false)
    }
    onConfirm: apply()

    tools: RatioButtons {
        prefix: "crop"
        look: root.look
        ratioName: root.ratioName
        turned: root.turnedRatio()
        onPicked: id => root.pickRatio(id)
    }

    CropView {
        id: picture
        objectName: "cropView"
        anchors.fill: parent
        look: root.look
        rect: root.draft
        ratio: root.ratio
        onCommitted: r => root.draft = r
    }

    // The fields at the start, Cancel and OK at the end (KitDialogButtons). An Item of a known height, not a layout: a layout's
    // height comes a frame late, and the picture above it was fitted again (a moved frame under a drag).
    footer: Item {
        width: parent ? parent.width : 0
        height: Math.max(cropFields.implicitHeight, 7 * root.look.unit)
        RectangleFields {
            id: cropFields
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            prefix: "crop"
            look: root.look
            values: root.draft
            onEdited: (field, text) => root.setField(field, text)
        }
        KitDialogButtons {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            ViewButton {
                objectName: "cropCancel"
                dialogRole: "cancel"
                look: root.look
                text: qsTr("Cancel")
                onTapped: root.close()
            }
            ViewButton {
                objectName: "cropOk"
                look: root.look
                text: qsTr("OK")
                primary: true
                onTapped: root.apply()
            }
        }
    }
}
