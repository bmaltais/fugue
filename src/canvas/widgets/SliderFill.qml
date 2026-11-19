import QtQuick

// A slider's share of its range (canvas-look 4.4): a fill from the left of the well, made
// only for slider widgets.
Rectangle {
    required property Item host
    visible: host.editable
    height: host.height
    width: visible ? host.width * host.edits.fraction(host.nodeId, host.name, host.number) : 0
    radius: host.smallCorner
    color: Qt.alpha(host.look.accent, 0.22)
}
