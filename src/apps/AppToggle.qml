import QtQuick
import Fugue

// A switch: a track and a knob, 28px tall with its label beside it; Space or Enter flips it, a click too. `checked` in,
// `toggled(checked)` out; the caller sets `checked` from its model.
Rectangle {
    id: sw
    required property CanvasLook look
    property bool checked: false
    property string label
    signal toggled(bool checked)

    implicitWidth: 22 * look.unit
    implicitHeight: 7 * look.unit
    color: "transparent"
    activeFocusOnTab: true
    Accessible.role: Accessible.CheckBox
    Accessible.name: label
    Accessible.checked: checked
    Accessible.onToggleAction: sw.toggled(!sw.checked)
    Accessible.onPressAction: sw.toggled(!sw.checked)
    Keys.onSpacePressed: toggled(!checked)
    Keys.onReturnPressed: toggled(!checked)
    Keys.onEnterPressed: toggled(!checked)

    Rectangle {
        id: track
        anchors.verticalCenter: parent.verticalCenter
        width: 8 * sw.look.unit
        height: 4.5 * sw.look.unit
        radius: height / 2
        color: sw.checked ? sw.look.accent : sw.look.hairlineStrong
        border.width: sw.activeFocus ? 2 : 0
        border.color: sw.look.accent
        Rectangle {
            x: sw.checked ? track.width - width - 2 : 2
            anchors.verticalCenter: parent.verticalCenter
            width: track.height - 4
            height: width
            radius: width / 2
            color: sw.checked ? sw.look.accentForeground : sw.look.body
        }
    }
    Text {
        anchors { left: track.right; leftMargin: 2 * sw.look.unit; verticalCenter: parent.verticalCenter }
        text: sw.checked ? qsTr("On") : qsTr("Off")
        color: sw.look.label
        font.pixelSize: sw.look.labelSize
    }
    TapHandler { onTapped: { sw.forceActiveFocus(); sw.toggled(!sw.checked) } }
}
