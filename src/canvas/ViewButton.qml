import QtQuick
import Fugue

// A button of a large view's bar and footer (#62, #73): 7u = 28px tall, above the 24px floor, a label
// on the well color; `current` marks the choice that holds (the accent, or the quiet marking for a
// zoom), `primary` is the surface's one highlighted action (OK). It takes Tab focus; Enter and Space
// press it. It knows nothing of what it does: `tapped` says it was pressed.
Rectangle {
    id: button
    required property CanvasLook look
    property string text
    property bool current: false
    property bool accented: true  // `current` shows in the accent; false in the quiet selection color
    property bool primary: false
    signal tapped

    readonly property bool marked: primary || (current && accented)
    implicitWidth: Math.max(label.implicitWidth + 4 * look.unit, implicitHeight)
    implicitHeight: 7 * look.unit
    width: implicitWidth
    height: implicitHeight
    radius: look.smallCorner
    color: marked ? look.accent : current ? look.selectedRow : look.well
    activeFocusOnTab: enabled
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.Button
    Accessible.name: text
    Text {
        id: label
        anchors.centerIn: parent
        text: button.text
        color: button.marked ? button.look.accentForeground : button.look.text
        font.pixelSize: button.look.labelSize
    }
    TapHandler { onTapped: button.tapped() }
    Keys.onReturnPressed: button.tapped()
    Keys.onSpacePressed: button.tapped()
}
