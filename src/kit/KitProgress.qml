import QtQuick
import Fugue
import "KitTone.js" as KitTone

// A progress bar of the Drift kit (docs/canvas-look.md "Drift kit"): the slider's recessed rail (`kitTrack`, a 1.5u groove)
// with a fill in the panel's tone, or the accent outside a panel, as the slider fills. It is not a control: no focus, no
// pointer, only what it shows, and assistive tools hear a progress bar named by `text` with the percent as its value.
//
// Determinate: `value` between `from` and `to` fills the rail from the start (the right in a right to left window). Indeterminate:
// a short bar of a third of the rail glides across and back, slowly, eased at both ends, while the bar is visible and its
// window is active. Under reduced motion (the `motion` look parameter, as KitVideo) nothing moves: a third of the rail rests
// filled in the middle, dimmer than a real value, so it is still told apart from 0% and from done.
Rectangle { // the recessed rail
    id: bar
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property real from: 0
    property real to: 1
    property real value: 0
    property bool indeterminate: false
    property string text // the name assistive tools say
    property string tone: KitTone.nearest(bar) // the panel's tone; "" is the accent

    readonly property color toneColor: tone === "" || look.params.controlColour === "accent" ? look.accent : look.tone(tone).glass
    readonly property bool glass: look.frost > 0.001
    readonly property bool mirrored: LayoutMirroring.enabled
    readonly property bool reduced: look.params.motion === "reduced"
    readonly property real fraction: to > from ? Math.max(0, Math.min(1, (value - from) / (to - from))) : 0
    readonly property bool moving: indeterminate && !reduced && visible && Window.active
    // Where the indeterminate bar is along its room, 0 to 1; it only moves while `moving`.
    property real sweep: 0.5

    implicitWidth: 40 * look.unit
    implicitHeight: 1.5 * look.unit
    width: implicitWidth
    height: implicitHeight
    radius: height / 2
    color: glass ? Qt.rgba(0, 0, 0, look.light ? look.params.glassTrackAlphaLight : look.params.glassTrackAlpha) : look.kitTrack
    border.width: glass ? 1 : 0
    border.color: Qt.rgba(0, 0, 0, look.params.trackEdge * (look.light ? 0.64 : 1))
    clip: true
    Accessible.role: Accessible.ProgressBar
    Accessible.name: text
    Accessible.description: indeterminate ? "" : Math.round(fraction * 100) + "%"

    onMovingChanged: if (!moving) sweep = 0.5

    SequentialAnimation on sweep {
        running: bar.moving
        loops: Animation.Infinite
        NumberAnimation { from: 0; to: 1; duration: 1600; easing.type: Easing.InOutSine }
        NumberAnimation { from: 1; to: 0; duration: 1600; easing.type: Easing.InOutSine }
    }

    Rectangle { // the fill, inside the rail
        readonly property real room: bar.width - 0.5 * bar.look.unit
        readonly property real span: bar.indeterminate ? room / 3 : room * bar.fraction
        x: (bar.mirrored ? bar.width - 0.25 * bar.look.unit - width : 0.25 * bar.look.unit)
           + (bar.indeterminate ? (bar.mirrored ? -1 : 1) * (room - span) * bar.sweep : 0)
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, span)
        height: Math.max(0, bar.height - 0.5 * bar.look.unit)
        radius: height / 2
        visible: width > 0
        opacity: bar.indeterminate && bar.reduced ? 0.55 : 1
        color: bar.glass ? Qt.alpha(bar.toneColor, bar.look.params.sliderFill) : Qt.tint(bar.look.kitTrack, Qt.alpha(bar.toneColor, 0.75))
    }
}
