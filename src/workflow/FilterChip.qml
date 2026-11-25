import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"

// A filter chip: a pill that toggles. Its state is said by the check and the accent edge as well as the fill. It only says
// that it was toggled (`toggled`); the owner keeps the state and sets `checked`. Space and Return toggle it, a tap too.
Rectangle {
    id: chip

    required property CanvasLook look
    property string text
    property string tip
    property bool checked: false
    signal toggled

    readonly property real u: look.unit

    width: row.implicitWidth + 4 * u
    height: 7 * u
    radius: height / 2
    color: checked ? look.selectedRow : hover.hovered ? look.well : "transparent"
    border.width: activeFocus ? 2 : 1
    border.color: checked || activeFocus ? look.accent : look.hairlineStrong
    activeFocusOnTab: true
    Accessible.role: Accessible.CheckBox
    Accessible.name: text
    Accessible.checked: checked
    Keys.onSpacePressed: toggled()
    Keys.onReturnPressed: toggled()
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: chip.toggled() }
    ToolTip.visible: hover.hovered && tip !== ""
    ToolTip.delay: App.theme.look.tooltipDelay
    ToolTip.text: tip
    Row {
        id: row
        anchors.centerIn: parent
        spacing: chip.u
        Glyph {
            visible: chip.checked
            anchors.verticalCenter: parent.verticalCenter
            look: chip.look
            name: "check"
            color: chip.look.accent
            font.pixelSize: 12
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: chip.text
            color: chip.look.text
            font.pixelSize: chip.look.textSize
        }
    }
}
