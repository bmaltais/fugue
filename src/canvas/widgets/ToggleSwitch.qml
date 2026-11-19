import QtQuick

// A toggle widget's switch at the right end of its well (canvas-look 4.4).
Rectangle {
    id: track
    objectName: "widgetToggle"
    required property Item host
    anchors.right: parent.right
    anchors.rightMargin: host.pad / 2
    anchors.verticalCenter: parent.verticalCenter
    width: 22
    height: 12
    radius: 6
    color: host.on ? host.look.accent : host.look.border
    Rectangle {
        x: track.host.on ? track.width - width - 2 : 2
        y: 2
        width: 8
        height: 8
        radius: 4
        color: track.host.on ? theme.accentForeground : track.host.look.panel
    }
}
