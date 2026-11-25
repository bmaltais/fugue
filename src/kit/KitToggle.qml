import QtQuick
import Fugue

// A pill switch (docs/canvas-look.md "Drift kit"): the accent track and a round knob when on, the `kitTrack` rail and
// the `kitKnob` when off. It is 10u by 7u to press (the 7u floor, docs/ui-ergonomics.md), the track 5.5u inside it.
// The value comes in through `checked` and the change goes out through `toggled(value)`: the switch never sets
// `checked` itself, so a binding stays a binding and the model decides. Space flips it on the keyboard, Tab reaches
// it; assistive tools hear a switch named by `text`. Put it in a KitRow's trailing slot and point the row's `target` at
// it, and a click anywhere on the row flips it too.
Item {
    id: control
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property bool checked: false
    property string text // the name assistive tools say; a KitRow with a label names its own switch
    signal toggled(bool value)

    // What a row's click calls: asks for the opposite value.
    function toggle() {
        if (enabled)
            toggled(!checked)
    }

    implicitWidth: 10 * look.unit
    implicitHeight: 7 * look.unit
    width: implicitWidth
    height: implicitHeight
    activeFocusOnTab: enabled
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.CheckBox
    Accessible.name: text
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.onPressAction: control.toggle()
    Accessible.onToggleAction: control.toggle()

    Rectangle {
        id: track
        anchors.centerIn: parent
        width: 10 * control.look.unit
        height: 5.5 * control.look.unit
        radius: height / 2
        color: control.checked ? control.look.accent : control.look.kitTrack
        border.width: control.activeFocus ? 2 : 0
        border.color: control.look.accent
        Behavior on color { ColorAnimation { duration: 100; easing.type: Easing.OutCubic } }
        Rectangle {
            readonly property real gap: 0.5 * control.look.unit
            width: track.height - 2 * gap
            height: width
            radius: width / 2
            anchors.verticalCenter: parent.verticalCenter
            x: control.checked ? track.width - width - gap : gap
            color: control.checked ? control.look.accentForeground : control.look.kitKnob
            Behavior on x { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
        }
    }
    // The press is the switch's own, so the row it sits in does not also flip it.
    TapHandler {
        gesturePolicy: TapHandler.WithinBounds
        onTapped: control.toggle()
    }
    HoverHandler { cursorShape: control.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
    Keys.onSpacePressed: control.toggle()
}
