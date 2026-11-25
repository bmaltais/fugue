import QtQuick
import Fugue
import "../kit"

// One input of the app panel as a row of a KitGroup (docs/canvas-look.md "Drift kit"), for the controls that are too wide to
// sit at a label's end: the label and its dim hint on top, the control below at the row's full width (the picture well, a prompt,
// a slider, a seed row). It draws the group's hairline above itself unless it is the first row. A small control that does fit
// at the end (a switch, a choice) is a KitRow instead (AppInput makes the right one).
Item {
    id: row
    property CanvasLook look: App.theme.look
    property string label
    property string hint
    default property alias content: slot.data

    width: parent ? parent.width : implicitWidth
    implicitHeight: column.implicitHeight + 6 * look.unit
    height: implicitHeight
    Accessible.role: Accessible.Grouping
    Accessible.name: label
    Accessible.description: hint

    KitSeparator {
        look: row.look
        visible: row.y > 0
        anchors.top: parent.top
    }
    Column {
        id: column
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter
                  leftMargin: 3.5 * row.look.unit; rightMargin: 3 * row.look.unit }
        spacing: row.look.unit
        Item {
            width: parent.width
            height: heading.implicitHeight + (row.hint !== "" ? hintText.implicitHeight + row.look.unit / 2 : 0)
            Text {
                id: heading
                anchors { left: parent.left; right: parent.right }
                text: row.label
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: row.look.text
                font.pixelSize: Math.round(row.look.textSize * 7 / 6)
            }
            Text {
                id: hintText
                anchors { left: parent.left; right: parent.right; top: heading.bottom; topMargin: row.look.unit / 2 }
                visible: row.hint !== ""
                text: row.hint
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: row.look.label
                font.pixelSize: row.look.labelSize
            }
        }
        Column {
            id: slot
            width: parent.width
        }
    }
}
