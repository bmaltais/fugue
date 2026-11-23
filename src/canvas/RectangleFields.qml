pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// The four number fields of a rectangle (X, Y, Width, Height) for a picture window's footer: the Crop window's and the
// region picker's (docs/ui-ergonomics.md, C10). `prefix` names the fields (`<prefix>Field-x`). `values` is the
// rectangle shown ({x, y, width, height}); a field is typed in, and `edited(field, text)` says one was entered or left
// (the host sets the rectangle, which comes back through `values`). A field being typed in keeps its text. It knows
// nothing of what the rectangle is for.
Row {
    id: root
    required property CanvasLook look
    property string prefix
    property var values: ({x: 0, y: 0, width: 0, height: 0})
    signal edited(string field, string text)
    spacing: 2 * look.unit

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
                objectName: root.prefix + "Field-" + entry.modelData.id
                width: 14 * root.look.unit
                enabled: root.enabled
                text: String(root.values[entry.modelData.id])
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 0; top: 99999 }
                selectByMouse: true
                // Typing edits the text; the rectangle follows when it is entered or left. Enter also goes on to the
                // window, which applies it (the field takes it first).
                onEditingFinished: {
                    root.edited(entry.modelData.id, text)
                    text = String(root.values[entry.modelData.id])
                }
                Connections {
                    target: root
                    function onValuesChanged() { if (!field.activeFocus) field.text = String(root.values[entry.modelData.id]) }
                }
            }
        }
    }
}
