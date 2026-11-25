import QtQuick
import QtQuick.Layouts
import Fugue
import "../canvas"

// A ranged number as a slider with its value shown and editable (the App form, ComfyUI App Mode's layout): a thin track with
// a handle, and at its end the number in a small field (Enter takes it, Escape puts it back). A press or drag on the track
// sets it, Left and Right step it (Shift: ten steps), Home and End go to the ends, a double click puts back `defaultValue`.
// It knows nothing of what it sets: `committed(v)` says each value chosen, the caller sets `value`.
RowLayout {
    id: root
    required property CanvasLook look
    property string label
    property real from: 0
    property real to: 1
    property real step: 1
    property real value: 0
    property real defaultValue: from
    signal committed(real v)

    readonly property int decimals: step >= 1 ? 0 : Math.max(1, Math.ceil(-Math.log(step) / Math.LN10))
    readonly property real fraction: to > from ? Math.max(0, Math.min(1, (value - from) / (to - from))) : 0
    function snap(v) {
        const stepped = from + Math.round((v - from) / step) * step
        return Math.max(from, Math.min(to, Number(stepped.toFixed(decimals))))
    }
    function choose(v) {
        const next = snap(v)
        if (Math.abs(next - value) > 1e-9)
            committed(next)
    }
    spacing: 2 * look.unit

    Item {
        id: track
        objectName: "appSliderTrack"
        Layout.fillWidth: true
        Layout.preferredHeight: 7 * root.look.unit
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: root.label
        Accessible.description: root.value.toFixed(root.decimals)
        Keys.onPressed: event => {
            const big = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
            if (event.key === Qt.Key_Left || event.key === Qt.Key_Down)
                root.choose(root.value - root.step * big * (Application.layoutDirection === Qt.RightToLeft && event.key === Qt.Key_Left ? -1 : 1))
            else if (event.key === Qt.Key_Right || event.key === Qt.Key_Up)
                root.choose(root.value + root.step * big * (Application.layoutDirection === Qt.RightToLeft && event.key === Qt.Key_Right ? -1 : 1))
            else if (event.key === Qt.Key_Home)
                root.choose(root.from)
            else if (event.key === Qt.Key_End)
                root.choose(root.to)
            else
                return
            event.accepted = true
        }
        Rectangle {
            id: rail
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
            anchors.leftMargin: handle.width / 2
            anchors.rightMargin: handle.width / 2
            height: 4
            radius: 2
            color: root.look.well
            border.width: 1
            border.color: root.look.hairline
            Rectangle {
                width: parent.width * root.fraction
                height: parent.height
                radius: 2
                color: root.look.accent
            }
        }
        Rectangle {
            id: handle
            width: 4 * root.look.unit
            height: width
            radius: width / 2
            x: rail.x + rail.width * root.fraction - width / 2
            anchors.verticalCenter: parent.verticalCenter
            color: root.look.text
            border.width: track.activeFocus ? 2 : 0
            border.color: root.look.accent
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            function at(x) { root.choose(root.from + (root.to - root.from) * Math.max(0, Math.min(1, (x - rail.x) / rail.width))) }
            onPressed: mouse => { track.forceActiveFocus(); at(mouse.x) }
            onPositionChanged: mouse => { if (pressed) at(mouse.x) }
            onDoubleClicked: root.choose(root.defaultValue)
        }
    }
    AppField {
        id: number
        objectName: "appSliderValue"
        look: root.look
        Layout.preferredWidth: 14 * root.look.unit
        label: root.label
        horizontalAlignment: TextInput.AlignRight
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
        text: root.value.toFixed(root.decimals)
        onEditingFinished: {
            const typed = Number(text.replace(",", "."))
            if (!isNaN(typed) && text.trim() !== "")
                root.choose(typed)
            text = Qt.binding(() => root.value.toFixed(root.decimals))
        }
        Keys.onEscapePressed: text = Qt.binding(() => root.value.toFixed(root.decimals))
    }
}
