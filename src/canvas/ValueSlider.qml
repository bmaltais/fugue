import QtQuick
import QtQuick.Controls
import Fugue

// A slider for a bar (the mask and paint editors', #172): a well with its fill from the left, the label
// and the value over it (the node's slider widget, canvas-look 4.4). A press or drag sets it, Left and
// Right step it (Shift: ten steps), the wheel too. 26u wide and 28px tall, above the 24px floor. It
// knows nothing of what it sets: `moved(v)` says what the pointer or a key asks for, and the caller
// sets `value`.
Rectangle {
    id: slider
    required property CanvasLook look
    property string label
    property string shown
    property string tip
    property real from: 0
    property real to: 1
    property real value: 0
    property real step: 1
    property bool curved: false // a square-law scale, finer at the small end
    signal moved(real v)
    readonly property real fraction: to > from ? Math.max(0, Math.min(1, (value - from) / (to - from))) : 0
    readonly property real position: curved ? Math.sqrt(fraction) : fraction
    function valueAt(x) {
        const f = Math.max(0, Math.min(1, x / width))
        return from + (to - from) * (curved ? f * f : f)
    }
    width: 26 * look.unit
    height: 7 * look.unit
    radius: look.smallCorner
    color: look.well
    border.width: activeFocus && FocusInput.keyboard ? 2 : 0
    border.color: look.accent
    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: label
    Accessible.description: shown
    Rectangle {
        width: slider.width * slider.position
        height: slider.height
        radius: slider.look.smallCorner
        color: Qt.alpha(slider.look.accent, 0.22)
    }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        x: 1.5 * slider.look.unit
        text: slider.label
        color: slider.look.label
        font.pixelSize: slider.look.labelSize
    }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: parent.right
        anchors.rightMargin: 1.5 * slider.look.unit
        text: slider.shown
        color: slider.look.text
        font.pixelSize: slider.look.labelSize
        font.features: { "tnum": 1 }
    }
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onPressed: mouse => { slider.forceActiveFocus(); slider.moved(slider.valueAt(mouse.x)) }
        onPositionChanged: mouse => { if (pressed) slider.moved(slider.valueAt(mouse.x)) }
        onWheel: wheel => slider.moved(slider.value + (wheel.angleDelta.y > 0 ? 1 : -1) * slider.step * (wheel.modifiers & Qt.ShiftModifier ? 10 : 5))
    }
    Keys.onPressed: event => {
        const amount = slider.step * (event.modifiers & Qt.ShiftModifier ? 10 : 1)
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Down) slider.moved(slider.value - amount)
        else if (event.key === Qt.Key_Right || event.key === Qt.Key_Up) slider.moved(slider.value + amount)
        else return
        event.accepted = true
    }
    ToolTip.visible: hover.hovered && slider.tip !== ""
    ToolTip.text: slider.tip
    ToolTip.delay: 600
    HoverHandler { id: hover }
}
