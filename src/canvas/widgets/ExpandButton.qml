import QtQuick
import ".."

// A multiline well's expand button (#715, canvas-look 4.4): Lucide `maximize-2`, 24 px, at the well's top right while the pointer is over
// the well (and over the button), which opens the text window on this input. The well's own row of text runs under it, so it has the
// well's fill behind it. Made on the first hover, like the spin chevrons.
Rectangle {
    id: expand
    objectName: "widgetExpand"
    required property InputWidget host
    required property MouseArea mouse
    readonly property real side: 24
    visible: (mouse.containsMouse || button.hovered) && !host.editingText && host.canvasView && host.canvasView.openText !== undefined
    x: host.width - side - 2
    y: 2
    width: side
    height: side
    z: 3
    radius: host.smallCorner
    color: host.wellColor
    ViewButton {
        id: button
        anchors.fill: parent
        look: expand.host.look
        compact: true
        flat: true
        glyph: "maximize-2"
        tip: qsTr("Edit in a larger window (Alt+T)")
        onTapped: expand.host.canvasView.openText(expand.host.nodeId, expand.host.name)
    }
}
