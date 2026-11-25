import QtQuick
import QtQuick.Layouts
import Fugue

// One row of a KitGroup (docs/canvas-look.md "Drift kit"): a label, an optional dim one-line hint under it, and
// whatever control belongs at the end, given as the row's children (a KitToggle, a KitChip, a KitSlider, a drop-down,
// or several in a row). It is at least 10u tall, taller by the hint. Above itself it draws the hairline, unless it is
// the group's first visible row. When the label and the controls do not fit side by side (a narrow panel), the row stacks: the
// label above, the controls under it at the start (`stacked`), so neither is squeezed and the margins stay even (owner, 2026-11-25).
//
// `target` is a control of the row with a `toggle()` function (a KitToggle): a click anywhere on the row, not only on
// the control, calls it, as in the settings of iOS and Drift. A click on the trailing controls belongs to them, never
// to the row. The row itself takes no focus; the control does. Without `target` the row does nothing on a click.
Item {
    id: row
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property string label
    property string hint
    property int hintLines: 1 // more than 1: the hint wraps to that many lines (then ends with an ellipsis)
    property var target // a control of the row with a toggle() function
    default property alias trailing: slot.data
    readonly property real innerWidth: width - (look.params.rowPadLeft + look.params.rowPadRight) * look.unit
    // The label keeps at least 16u (or its whole width when shorter) beside the controls; less than that and the row stacks.
    readonly property bool stacked: slot.implicitWidth > 0
                                    && Math.min(labelText.implicitWidth, 16 * look.unit) + look.params.rowGap * look.unit + slot.implicitWidth > innerWidth

    width: parent ? parent.width : implicitWidth
    implicitHeight: Math.max(look.params.rowMinHeight * look.unit, layout.implicitHeight + 2 * look.params.rowPadY * look.unit)
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
    GridLayout {
        id: layout
        columns: row.stacked ? 1 : 2
        rowSpacing: 1.5 * row.look.unit
        columnSpacing: row.look.params.rowGap * row.look.unit
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: row.look.params.rowPadLeft * row.look.unit
        anchors.rightMargin: row.look.params.rowPadRight * row.look.unit
        Column {
            Layout.fillWidth: true
            spacing: 0.5 * row.look.unit
            Text {
                id: labelText
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
                wrapMode: row.hintLines > 1 ? Text.Wrap : Text.NoWrap
                maximumLineCount: row.hintLines
                elide: Text.ElideRight
                color: row.look.label
                font.pixelSize: row.look.labelSize
            }
        }
        Row {
            id: slot
            Layout.alignment: row.stacked ? Qt.AlignLeft | Qt.AlignVCenter : Qt.AlignVCenter
            spacing: 2 * row.look.unit
        }
    }
}
