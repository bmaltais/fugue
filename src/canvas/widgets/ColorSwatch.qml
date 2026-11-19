import QtQuick

// A color widget's swatch at the right end of its well (canvas-look 4.4); a click on it opens
// the color picker.
Rectangle {
    required property Item host
    anchors.right: parent.right
    anchors.rightMargin: (host.glyphZone - width) / 2
    anchors.verticalCenter: parent.verticalCenter
    width: 12
    height: 12
    radius: host.smallCorner
    color: host.colorOf(host.valueText, "transparent")
    border.width: 1
    border.color: host.look.hairlineStrong
}
