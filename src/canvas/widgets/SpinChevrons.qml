import QtQuick
import ".."

// A spin widget's ‹ › at the ends of its well while the pointer is over it (canvas-look 4.4):
// a click there steps the value. Made on the first hover.
Item {
    id: chevrons
    required property Item host
    required property MouseArea mouse
    anchors.fill: parent
    visible: mouse.containsMouse && !host.editingText
    z: 1

    Glyph {
        x: 2
        anchors.verticalCenter: parent.verticalCenter
        name: "chevron-left"
        look: chevrons.host.look
        color: chevrons.host.look.label
        font.pixelSize: 12
    }
    Glyph {
        anchors.right: parent.right
        anchors.rightMargin: 2
        anchors.verticalCenter: parent.verticalCenter
        name: "chevron-right"
        look: chevrons.host.look
        color: chevrons.host.look.label
        font.pixelSize: 12
    }
}
