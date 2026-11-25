pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../canvas"
import "KitTone.js" as KitTone

// A round-thumb slider of the Drift kit (docs/canvas-look.md "Drift kit"): a 1u rail on `kitTrack`, filled to the
// thumb in the accent, a 4.5u `kitKnob` thumb, optional tick dots under the rail, optional Lucide glyphs at the
// start and the end (a turtle and a rabbit for a speed), and an optional value at the end. It is 7u tall, the floor.
//
// The value comes in through `value` and goes out through `moved(value)` on a press, a drag or a key: the slider never
// sets `value` itself, so a binding stays a binding and the model decides what to keep. A `stepSize` above 0 snaps
// to steps from `from` (and `ticks` draws one dot at each, up to 24 of them). Keys, when focused: arrows move one step
// (a fortieth of the range when `stepSize` is 0), PageUp and PageDown ten, Home and End the ends; in a right to left
// window Left and Right swap with the rail. With `showValue` the end slot is the value as a small recessed field the person can
// type in (owner, 2026-11-25: "the values should be input fields the user can edit"): Enter or leaving it takes the number,
// kept in the range and snapped to the steps, through `moved`; Escape puts the shown value back; Up and Down step it. The
// value is shown times `displayScale` with `suffix` (100 and "%" for a 0..1 amount), whole for whole steps, else to 2
// places; `valueText` is that text. The end slot has a fixed width (`endWidth`), so every rail in a group starts and ends at
// the same x.
Item {
    id: slider
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property real from: 0
    property real to: 1
    property real value: 0
    property real stepSize: 0
    property bool ticks: false
    property string startGlyph
    property string endGlyph
    property bool showValue: false
    property real displayScale: 1
    property string suffix
    readonly property bool wholeSteps: stepSize > 0 && Math.abs(stepSize * displayScale - Math.round(stepSize * displayScale)) < 1e-9
    readonly property string valueText: (wholeSteps ? String(Math.round(value * displayScale))
                                                    : String(Math.round(value * displayScale * 100) / 100)) + suffix
    property string text // the name assistive tools say; a KitRow with a label names its own slider
    property real railWidth: 40 * look.unit
    property real endWidth: (showValue && endGlyph === "" ? 11 : 6.5) * look.unit // a typed value needs room for "100%"
    property string tone: KitTone.nearest(slider) // the panel's tone; "" is the accent
    signal moved(real value)

    readonly property color toneColor: tone === "" || look.params.controlColour === "accent" ? look.accent : look.tone(tone).glass
    readonly property bool glass: look.frost > 0.001
    readonly property bool mirrored: LayoutMirroring.enabled
    readonly property real fraction: to > from ? Math.max(0, Math.min(1, (value - from) / (to - from))) : 0
    readonly property real thumbSize: 4.5 * look.unit
    readonly property real step: stepSize > 0 ? stepSize : (to - from) / 40
    readonly property int tickCount: stepSize > 0 ? Math.round((to - from) / stepSize) + 1 : 0

    // The value a position along the rail stands for, snapped to the steps.
    function valueAt(x) {
        const f = Math.max(0, Math.min(1, (x - thumbSize / 2) / (rail.width - thumbSize)))
        const v = from + (mirrored ? 1 - f : f) * (to - from)
        return clamp(stepSize > 0 ? from + Math.round((v - from) / stepSize) * stepSize : v)
    }
    function clamp(v) { return Math.max(Math.min(from, to), Math.min(Math.max(from, to), v)) }
    function nudge(steps) { moved(clamp(value + steps * step)) }
    // A typed value: a number (a comma as the decimal point too, the suffix optional), in the range and on the steps; anything
    // else changes nothing.
    function typed(text) {
        const n = parseFloat(String(text).replace(suffix, "").replace(",", ".").trim())
        if (isNaN(n))
            return
        const v = clamp(n / displayScale)
        moved(stepSize > 0 ? clamp(from + Math.round((v - from) / stepSize) * stepSize) : v)
    }

    implicitWidth: (startGlyph !== "" ? endWidth + look.unit : 0) + railWidth + (endGlyph !== "" || showValue ? look.unit + endWidth : 0)
    implicitHeight: 7 * look.unit
    width: implicitWidth
    height: implicitHeight
    activeFocusOnTab: enabled
    // The focus ring is for the keyboard (KitSegmented): a press marks the focus it gives, losing the focus clears the mark.
    property bool pressFocus: false
    onActiveFocusChanged: if (!activeFocus) pressFocus = false
    readonly property bool ring: activeFocus && !pressFocus
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.Slider
    Accessible.name: text
    Accessible.description: valueText
    Accessible.onIncreaseAction: slider.nudge(1)
    Accessible.onDecreaseAction: slider.nudge(-1)

    // Start glyph, rail, end slot: a Row, so a right to left window mirrors it as it mirrors the rail itself.
    Row {
        anchors.fill: parent
        spacing: slider.look.unit
        Item {
            visible: slider.startGlyph !== ""
            width: slider.endWidth
            height: parent.height
            Glyph {
                look: slider.look
                name: slider.startGlyph
                color: slider.look.label
                font.pixelSize: Math.round(slider.look.textSize * 7 / 6)
                anchors.centerIn: parent
            }
        }
    Item {
        id: rail
        width: slider.railWidth
        height: parent.height
        Rectangle { // the recessed rail
            id: groove
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 1.5 * slider.look.unit
            radius: height / 2
            color: slider.glass ? Qt.rgba(0, 0, 0, slider.look.light ? slider.look.params.glassTrackAlphaLight : slider.look.params.glassTrackAlpha) : slider.look.kitTrack
            border.width: slider.glass ? 1 : 0
            border.color: Qt.rgba(0, 0, 0, slider.look.params.trackEdge * (slider.look.light ? 0.64 : 1))
            Rectangle { // the fill, the tone, inside the rail
                id: fill
                x: slider.mirrored ? parent.width - width - 0.25 * slider.look.unit : 0.25 * slider.look.unit
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(0, slider.thumbSize / 2 + (parent.width - slider.thumbSize) * slider.fraction - 0.25 * slider.look.unit)
                height: slider.look.unit
                radius: height / 2
                color: slider.glass ? Qt.alpha(slider.toneColor, slider.look.params.sliderFill) : Qt.tint(slider.look.kitTrack, Qt.alpha(slider.toneColor, 0.75))
            }
        }
        KitHalo { // a hint of the tone along the filled part
            look: slider.look
            tone: slider.toneColor
            lit: slider.fraction > 0.02 && slider.glass
            boxWidth: Math.max(fill.width + 20 * slider.look.unit, 20 * slider.look.unit)
            boxHeight: 14 * slider.look.unit
            centerX: slider.mirrored ? groove.width - fill.width / 2 : fill.width / 2
            centerY: groove.y + groove.height / 2
        }
        Repeater {
            model: slider.ticks && slider.tickCount > 1 && slider.tickCount <= 24 ? slider.tickCount : 0
            Rectangle {
                required property int index
                readonly property real f: index / (slider.tickCount - 1)
                x: slider.thumbSize / 2 + (rail.width - slider.thumbSize) * (slider.mirrored ? 1 - f : f) - width / 2
                y: parent.height / 2 + 3 * slider.look.unit - height / 2
                width: 0.5 * slider.look.unit
                height: width
                radius: width / 2
                color: slider.look.kitTrack
            }
        }
        Rectangle {
            id: thumb
            readonly property real along: (rail.width - width) * (slider.mirrored ? 1 - slider.fraction : slider.fraction)
            x: along
            anchors.verticalCenter: parent.verticalCenter
            width: slider.thumbSize
            height: width
            radius: width / 2
            color: slider.look.kitKnob
            // The ring is for the keyboard: a press focuses the slider (the arrows then move it) but shows none (KitSegmented).
            border.width: slider.ring ? 2 : slider.look.light ? 1 : 0
            border.color: slider.ring ? slider.look.accent : slider.look.hairlineStrong
            Rectangle { // the thumb's drop shadow
                anchors { fill: parent; topMargin: slider.look.params.raiseShadowOffset; bottomMargin: -slider.look.params.raiseShadowOffset }
                z: -1
                radius: width / 2
                color: Qt.rgba(0, 0, 0, slider.look.light ? 0.25 : 0.45)
                visible: slider.look.params.raiseShadowOffset > 0.01
            }
        }
        // A press or a drag anywhere on the rail's row asks for the value under the pointer.
        DragHandler {
            id: drag
            target: null
            dragThreshold: 0
            onCentroidChanged: if (active) slider.moved(slider.valueAt(centroid.position.x))
        }
        TapHandler {
            id: tap
            gesturePolicy: TapHandler.WithinBounds
            onPressedChanged: if (pressed) { if (!slider.activeFocus) slider.pressFocus = true; slider.forceActiveFocus(Qt.MouseFocusReason); slider.moved(slider.valueAt(tap.point.position.x)) }
        }
        HoverHandler { cursorShape: slider.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
    }
    // The end slot: a glyph, or the value, at one width for every slider of a group.
    Item {
        id: endSlot
        width: slider.endWidth
        height: parent.height
        visible: slider.endGlyph !== "" || slider.showValue
        Glyph {
            visible: slider.endGlyph !== ""
            look: slider.look
            name: slider.endGlyph
            color: slider.look.label
            font.pixelSize: Math.round(slider.look.textSize * 7 / 6)
            anchors.centerIn: parent
        }
        Item { // the value, a field to type in
            visible: slider.endGlyph === "" && slider.showValue
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 6 * slider.look.unit
            KitWell { look: slider.look; focused: valueField.activeFocus }
            TextInput {
                id: valueField
                objectName: slider.objectName !== "" ? slider.objectName + "-value" : ""
                anchors { fill: parent; leftMargin: slider.look.unit; rightMargin: slider.look.unit }
                verticalAlignment: TextInput.AlignVCenter
                horizontalAlignment: TextInput.AlignRight
                clip: true
                selectByMouse: true
                activeFocusOnTab: slider.enabled
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                color: slider.look.text
                selectionColor: slider.look.selectedRow
                selectedTextColor: slider.look.text
                font.pixelSize: slider.look.textSize
                font.features: { "tnum": 1 }
                Accessible.role: Accessible.EditableText
                Accessible.name: slider.text
                // The shown value while the field is not being typed in.
                Binding on text { value: slider.valueText; when: !valueField.activeFocus; restoreMode: Binding.RestoreNone }
                onActiveFocusChanged: if (activeFocus) selectAll()
                onEditingFinished: {
                    slider.typed(text)
                    Qt.callLater(() => { valueField.text = slider.valueText; if (valueField.activeFocus) valueField.selectAll() })
                }
                Keys.onEscapePressed: event => { valueField.text = slider.valueText; valueField.selectAll(); event.accepted = true }
                Keys.onUpPressed: slider.nudge(1)
                Keys.onDownPressed: slider.nudge(-1)
            }
        }
    }
    }
    Keys.onPressed: function (event) {
        const dir = slider.mirrored ? -1 : 1
        switch (event.key) {
        case Qt.Key_Right: case Qt.Key_Up: slider.nudge(event.key === Qt.Key_Right ? dir : 1); break
        case Qt.Key_Left: case Qt.Key_Down: slider.nudge(event.key === Qt.Key_Left ? -dir : -1); break
        case Qt.Key_PageUp: slider.nudge(10); break
        case Qt.Key_PageDown: slider.nudge(-10); break
        case Qt.Key_Home: slider.moved(slider.from); break
        case Qt.Key_End: slider.moved(slider.to); break
        default: return
        }
        event.accepted = true
    }
}
