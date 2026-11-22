pragma ComponentBehavior: Bound
import QtQuick
import Fugue

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

    tools: Row {
        spacing: root.look.unit
        Repeater {
            model: [{id: "free", text: qsTr("Free")}, {id: "source", text: qsTr("Original")}, {id: "1:1", text: "1:1"},
                    {id: "4:3", text: "4:3"}, {id: "3:2", text: "3:2"}, {id: "16:9", text: "16:9"}]
            delegate: ViewButton {
                required property var modelData
                objectName: "cropRatio-" + modelData.id
                look: root.look
                text: modelData.text
                accented: false // the chosen ratio is the quiet marking: OK is the one accent fill (D7)
                current: root.ratioName === modelData.id
                onTapped: root.pickRatio(modelData.id)
            }
        }
        ViewButton {
            objectName: "cropRatioTurn"
            look: root.look
            text: qsTr("Turn")
            enabled: root.turnedRatio() !== ""
            onTapped: root.pickRatio(root.turnedRatio())
        }
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

    footer: Row {
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
                    objectName: "cropField-" + entry.modelData.id
                    width: 14 * root.look.unit
                    text: String(root.draft[entry.modelData.id])
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { bottom: 0; top: 99999 }
                    selectByMouse: true
                    // Typing edits the text; the frame follows when it is entered or left. Enter also
                    // goes on to the window, which applies it (the field takes it first).
                    onEditingFinished: {
                        root.setField(entry.modelData.id, text)
                        text = String(root.draft[entry.modelData.id])
                    }
                    Connections {
                        target: root
                        function onDraftChanged() { if (!field.activeFocus) field.text = String(root.draft[entry.modelData.id]) }
                    }
                }
            }
        }
        Item { width: 2 * root.look.unit; height: 1 }
        ViewButton {
            objectName: "cropCancel"
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
