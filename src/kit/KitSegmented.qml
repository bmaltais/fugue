pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "KitTone.js" as KitTone

// A chooser for a few exclusive options (docs/canvas-look.md 20.9): the seed's Fixed, Random and Increment, the window's App | Graph.
// A recessed glass track with a raised translucent thumb in the window's tone sliding to the chosen option. `options` is a list of
// texts, `currentIndex` the chosen one; the control never sets it: a press or a key asks with `moved(index)` and the model decides.
// Keys, one tab stop: Left and Right (mirrored right to left) move to the next option and choose it, Home and End the ends; Space and
// Enter keep the choice. Each option is at least 7u wide to press and the widest text sets every option's width; a wider `width` is
// shared out equally. With Frost at 0 the track and thumb are opaque. Assistive tools hear a radio group named by `text`.
Item {
    id: seg
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property var options: []
    property var optionNames: [] // an object name for each option, for a test or a tool that presses one
    property int currentIndex: 0
    property string text // the name assistive tools say
    property string tone: KitTone.nearest(seg) // the panel's tone; "" is the accent
    signal moved(int index)

    readonly property color toneColor: tone === "" || look.params.controlColour === "accent" ? look.accent : look.tone(tone).glass
    readonly property bool glass: look.frost > 0.001
    readonly property var pm: look.params
    readonly property real pad: 0.75 * look.unit
    readonly property real optionHeight: 5.5 * look.unit
    readonly property int count: options.length
    // The widest option text sets every option's width; a wider item shares the rest out.
    readonly property real naturalOption: Math.max(7 * look.unit, widest.implicitWidth + 6 * look.unit)
    readonly property real optionWidth: count > 0 ? Math.max(naturalOption, (width - 2 * pad) / count) : naturalOption
    readonly property bool mirrored: LayoutMirroring.enabled

    implicitWidth: count * naturalOption + 2 * pad
    implicitHeight: 7 * look.unit
    width: implicitWidth
    height: implicitHeight
    activeFocusOnTab: enabled
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.RadioButton
    Accessible.name: text
    Accessible.description: currentIndex >= 0 && currentIndex < count ? String(options[currentIndex]) : ""

    function choose(index) {
        const next = Math.max(0, Math.min(count - 1, index))
        if (next !== currentIndex)
            moved(next)
    }

    Text { // measures the widest option; never seen
        id: widest
        visible: false
        text: seg.options.reduce((a, b) => String(b).length > a.length ? String(b) : a, "")
        font.pixelSize: seg.look.labelSize + 1
        font.weight: Font.Medium
    }
    Rectangle {
        id: track
        anchors.fill: parent
        radius: Math.min(seg.pm.cornerSegment * seg.look.unit, 0.5 * height)
        color: seg.glass ? Qt.rgba(0, 0, 0, seg.look.light ? seg.pm.glassTrackAlphaLight : seg.pm.glassTrackAlpha) : seg.look.kitTrack
        border.width: seg.activeFocus ? 2 : seg.glass ? 1 : 0
        border.color: seg.activeFocus ? seg.look.accent : Qt.rgba(0, 0, 0, seg.pm.trackEdge * (seg.look.light ? 0.64 : 1))
        Rectangle { // the inner shadow along the top edge
            visible: seg.glass && height > 0.01
            x: track.radius * 0.6
            y: 1
            width: parent.width - 2 * x
            height: seg.pm.wellShadowHeight
            color: Qt.rgba(0, 0, 0, seg.look.light ? seg.pm.wellShadowAlphaLight : seg.pm.glassTrackShadow * seg.pm.wellShadowAlpha / 0.22)
        }
        Rectangle {
            id: thumb
            readonly property int slot: seg.currentIndex
            x: seg.pad + (seg.mirrored ? seg.count - 1 - slot : slot) * seg.optionWidth
            y: (parent.height - height) / 2
            width: seg.optionWidth
            height: seg.optionHeight
            radius: Math.min(seg.pm.cornerSegment * seg.look.unit, 0.5 * height)
            visible: seg.currentIndex >= 0 && seg.currentIndex < seg.count
            gradient: Gradient {
                GradientStop { position: 0; color: seg.glass ? Qt.alpha(seg.toneColor, seg.pm.segThumbTop) : seg.toneColor }
                GradientStop { position: 1; color: seg.glass ? Qt.alpha(seg.toneColor, seg.pm.segThumbBottom) : seg.toneColor }
            }
            border.width: seg.glass ? 1 : 0
            border.color: Qt.rgba(1, 1, 1, 0.28)
            Behavior on x { NumberAnimation { duration: seg.pm.durThumb; easing.type: Easing.InOutQuad } }
            Behavior on width { NumberAnimation { duration: seg.pm.durThumb; easing.type: Easing.InOutQuad } }
            Rectangle { // the light along its top edge
                visible: seg.glass
                x: parent.radius * 0.5
                y: 1
                width: parent.width - 2 * x
                height: 1
                color: Qt.rgba(1, 1, 1, seg.pm.thumbShine)
            }
        }
        Row {
            x: seg.pad
            y: (parent.height - seg.optionHeight) / 2
            layoutDirection: seg.mirrored ? Qt.RightToLeft : Qt.LeftToRight
            Repeater {
                model: seg.options
                delegate: Item {
                    id: option
                    required property int index
                    required property var modelData
                    readonly property bool chosen: index === seg.currentIndex
                    objectName: seg.optionNames[index] ?? ""
                    width: seg.optionWidth
                    height: seg.optionHeight
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: String(modelData)
                    Accessible.checked: chosen
                    Text {
                        anchors.centerIn: parent
                        text: String(option.modelData)
                        textFormat: Text.PlainText
                        color: !seg.glass ? (option.chosen ? (seg.toneColor.hslLightness > 0.6 ? "#17171b" : "white") : seg.look.label)
                             : seg.look.light ? (option.chosen ? seg.look.text : seg.look.label)
                                                             : (option.chosen ? "white" : Qt.rgba(1, 1, 1, 0.72))
                        font.pixelSize: seg.look.labelSize + 1
                        font.weight: Font.Medium
                    }
                    TapHandler {
                        gesturePolicy: TapHandler.WithinBounds
                        onTapped: { seg.forceActiveFocus(Qt.MouseFocusReason); seg.choose(option.index) }
                    }
                }
            }
        }
    }
    HoverHandler { cursorShape: seg.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
    Keys.onPressed: function (event) {
        const dir = seg.mirrored ? -1 : 1
        switch (event.key) {
        case Qt.Key_Right: seg.choose(seg.currentIndex + dir); break
        case Qt.Key_Left: seg.choose(seg.currentIndex - dir); break
        case Qt.Key_Home: seg.choose(0); break
        case Qt.Key_End: seg.choose(seg.count - 1); break
        case Qt.Key_Space: case Qt.Key_Return: case Qt.Key_Enter: break // the choice stays
        default: return
        }
        event.accepted = true
    }
}
