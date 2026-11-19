import QtQuick

// A spin widget's ‹ › at the ends of its well while the pointer is over it (canvas-look 4.4):
// a click there steps the value. Made on the first hover.
Item {
    id: chevrons
    required property Item host
    required property MouseArea mouse
    anchors.fill: parent
    visible: mouse.containsMouse && !host.editingText
    z: 1

    Text {
        x: 2
        height: chevrons.height
        verticalAlignment: Text.AlignVCenter
        text: "‹"
        color: chevrons.host.look.muted
        font.pixelSize: chevrons.host.look.textSize
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: 2
        height: chevrons.height
        verticalAlignment: Text.AlignVCenter
        text: "›"
        color: chevrons.host.look.muted
        font.pixelSize: chevrons.host.look.textSize
    }
}
