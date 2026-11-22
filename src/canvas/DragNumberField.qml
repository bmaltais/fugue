import QtQuick
import Fugue

// A number as Blender's property fields have it (#226): a well with the label at the left, the value at the right and
// a fill for its share of the range. Drag left or right to scrub it (Shift: ten times finer, Ctrl: snap to `snap`),
// click to type it (Enter takes it, Escape puts it back), double-click to put back `defaultValue`. Left and Right step
// it when it has the focus. The value shown follows the pointer while it is dragged; `committed(v)` says once, on
// release or Enter, what the person chose, so a caller that makes a revision per change makes one per gesture. It knows
// nothing of what it sets: the caller sets `value` from its model. The InputWidget of a node's face does the same for a
// node's inputs but is tied to WidgetEdit; this one takes plain numbers. 28px tall, above the 24px floor.
Rectangle {
    id: field
    required property CanvasLook look
    property string label
    property string tip
    property real value: 0
    property real from: 0
    property real to: 1
    property real step: 0.01
    property real snap: 0.1
    property real defaultValue: 1
    // How a value is written ("40%"), and read back from what is typed (NaN when it is not a number).
    property var format: v => String(Math.round(v * 100) / 100)
    property var parse: text => Number(text)
    signal committed(real v)

    readonly property bool scrubbing: mouse.dragging
    readonly property real shownValue: mouse.dragging ? mouse.liveValue : value
    readonly property real fraction: to > from ? Math.max(0, Math.min(1, (shownValue - from) / (to - from))) : 0

    width: 40 * look.unit
    height: 7 * look.unit
    radius: look.smallCorner
    color: look.well
    border.width: activeFocus || editor.visible ? 1 : 0
    border.color: look.accent
    opacity: enabled ? 1 : 0.5
    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: label
    Accessible.description: tip

    function clamp(v) { return Math.max(from, Math.min(to, v)) }
    function commit(v) {
        const c = clamp(v)
        if (Math.abs(c - value) > 1e-9)
            committed(c)
    }
    function openEditor() {
        editor.text = format(value)
        editor.visible = true
        editor.forceActiveFocus()
        editor.selectAll()
    }

    Keys.onPressed: event => {
        const k = event.modifiers & Qt.ShiftModifier ? 10 : 1
        if (event.key === Qt.Key_Left) field.commit(field.value - field.step * k)
        else if (event.key === Qt.Key_Right) field.commit(field.value + field.step * k)
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) field.openEditor()
        else return
        event.accepted = true
    }

    Rectangle {
        height: parent.height
        width: parent.width * field.fraction
        radius: field.radius
        color: Qt.alpha(field.look.accent, 0.22)
        visible: !editor.visible
    }
    Text {
        anchors.left: parent.left
        anchors.leftMargin: 2 * field.look.unit
        anchors.verticalCenter: parent.verticalCenter
        visible: !editor.visible
        text: field.label
        color: field.look.label
        font.pixelSize: field.look.labelSize
    }
    Text {
        objectName: "dragNumberValue"
        anchors.right: parent.right
        anchors.rightMargin: 2 * field.look.unit
        anchors.verticalCenter: parent.verticalCenter
        visible: !editor.visible
        text: field.format(field.shownValue)
        color: field.look.text
        font.pixelSize: field.look.labelSize
        font.weight: Font.Medium
        font.features: { "tnum": 1 }
    }
    TextInput {
        id: editor
        objectName: "dragNumberEditor"
        visible: false
        anchors.fill: parent
        anchors.leftMargin: 2 * field.look.unit
        anchors.rightMargin: 2 * field.look.unit
        verticalAlignment: TextInput.AlignVCenter
        horizontalAlignment: TextInput.AlignRight
        selectByMouse: true
        clip: true
        color: field.look.text
        selectionColor: field.look.accent
        selectedTextColor: field.look.accentForeground
        font.pixelSize: field.look.labelSize
        onAccepted: {
            const v = field.parse(text)
            visible = false
            field.forceActiveFocus()
            if (!isNaN(v))
                field.commit(v)
        }
        Keys.onEscapePressed: event => {
            visible = false
            field.forceActiveFocus()
            event.accepted = true
        }
        onActiveFocusChanged: if (!activeFocus) visible = false
    }

    // A click waits for a second one: one opens the editor, two put the default back.
    Timer {
        id: clickWait
        interval: Application.styleHints.mouseDoubleClickInterval
        onTriggered: field.openEditor()
    }

    MouseArea {
        id: mouse
        objectName: "dragNumberMouse"
        anchors.fill: parent
        enabled: field.enabled && !editor.visible
        preventStealing: true
        cursorShape: Qt.SizeHorCursor
        property real pressX: 0
        property real startValue: 0
        property real liveValue: 0
        property bool dragging: false
        onPressed: event => {
            field.forceActiveFocus()
            pressX = event.x
            startValue = field.value
            liveValue = field.value
            dragging = false
        }
        onPositionChanged: event => {
            if (!pressed)
                return
            if (!dragging && Math.abs(event.x - pressX) < Application.styleHints.startDragDistance)
                return
            dragging = true
            // The whole range over 200 px; Shift ten times finer, Ctrl on steps of `snap`.
            const fine = event.modifiers & Qt.ShiftModifier ? 0.1 : 1
            let v = startValue + (event.x - pressX) / (50 * field.look.unit) * (field.to - field.from) * fine
            if (event.modifiers & Qt.ControlModifier)
                v = Math.round(v / field.snap) * field.snap
            liveValue = field.clamp(Math.round(v / field.step) * field.step)
        }
        onReleased: {
            if (dragging) {
                dragging = false
                field.commit(liveValue)
                return
            }
            if (clickWait.running) {
                clickWait.stop()
                field.commit(field.defaultValue)
            } else {
                clickWait.start()
            }
        }
        onCanceled: dragging = false
    }
}
