import QtQuick

// A toggle widget's switch at the right end of its well (canvas-look 4.4). Compact (docs/canvas-look.md 20.14): the kit's pill, 32 x 18
// with a 14px knob, in the tone when on and the kit's rail when off. KitToggle itself is not used on a node's face: it takes its own
// presses and carries a halo, and the canvas takes the presses of a node (the widget's mouse area calls `clicked`), with up to
// hundreds of nodes in a graph; this draws the same track and knob from the same look parameters in three items.
Rectangle {
    id: track
    objectName: "widgetToggle"
    required property InputWidget host
    readonly property bool compact: host.compact
    readonly property var pm: host.look.params
    anchors.right: parent.right
    anchors.rightMargin: compact ? 0 : host.pad / 2
    anchors.verticalCenter: parent.verticalCenter
    width: compact ? 32 : 22
    height: compact ? 18 : 12
    radius: height / 2
    color: compact ? (host.on ? Qt.alpha(host.toneColor, pm.toggleOnTop) : host.look.kitTrack)
                   : host.on ? host.look.accent : host.look.hairlineStrong
    border.width: compact && !host.on ? 1 : 0
    border.color: Qt.rgba(0, 0, 0, host.look.light ? 0.1 : 0.25)
    Rectangle {
        x: track.host.on ? track.width - width - (track.compact ? 2 : 2) : 2
        y: 2
        width: track.compact ? 14 : 8
        height: width
        radius: width / 2
        color: track.compact ? track.host.look.kitKnob : track.host.on ? track.host.look.accentForeground : track.host.look.body
        Behavior on x { enabled: track.compact; NumberAnimation { duration: track.pm.durToggle; easing.type: Easing.InOutQuad } }
    }
}
