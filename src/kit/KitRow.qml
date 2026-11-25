import QtQuick
import QtQuick.Layouts
import Fugue

// One row of a KitGroup (docs/canvas-look.md "Drift kit"): a label, an optional dim one-line hint under it, and
// whatever control belongs at the end, given as the row's children (a KitToggle, a KitChip, a KitSlider, a drop-down,
// or several in a row). It is at least 10u tall, taller by the hint. Above itself it draws the hairline, unless it is
// the group's first visible row.
//
// `target` is a control of the row with a `toggle()` function (a KitToggle): a click anywhere on the row, not only on
// the control, calls it, as in the settings of iOS and Drift. A click on the trailing controls belongs to them, never
// to the row. The row itself takes no focus; the control does. Without `target` the row does nothing on a click.
Item {
    id: row
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property string label
    property string hint
    property var target // a control of the row with a toggle() function
    default property alias trailing: slot.data

    width: parent ? parent.width : implicitWidth
    implicitHeight: Math.max(10 * look.unit, layout.implicitHeight + 6 * look.unit)
    height: implicitHeight
    Accessible.role: Accessible.Grouping
    Accessible.name: label
    Accessible.description: hint

    TapHandler {
        id: tap
        enabled: row.target !== null
        gesturePolicy: TapHandler.WithinBounds
        onTapped: function (point) {
            if (!row.target || !row.enabled)
                return
            const inSlot = slot.mapFromItem(row, point.position)
            // The trailing controls take their own taps.
            if (inSlot.x >= 0 && inSlot.x <= slot.width && inSlot.y >= 0 && inSlot.y <= slot.height)
                return
            row.target.toggle()
        }
    }
    KitSeparator {
        look: row.look
        visible: row.y > 0
        anchors.top: parent.top
    }
    RowLayout {
        id: layout
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 3.5 * row.look.unit
        anchors.rightMargin: 3 * row.look.unit
        spacing: 2 * row.look.unit
        Column {
            Layout.fillWidth: true
            spacing: 0.5 * row.look.unit
            Text {
                width: parent.width
                text: row.label
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: row.look.text
                font.pixelSize: Math.round(row.look.textSize * 7 / 6)
            }
            Text {
                visible: row.hint !== ""
                width: parent.width
                text: row.hint
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: row.look.label
                font.pixelSize: row.look.labelSize
            }
        }
        Row {
            id: slot
            Layout.alignment: Qt.AlignVCenter
            spacing: 2 * row.look.unit
        }
    }
}
