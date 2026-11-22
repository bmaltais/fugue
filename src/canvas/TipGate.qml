import QtQuick
import QtQuick.Controls

// Keeps the tooltips of a scrolling view's rows away while the view scrolls under a resting
// pointer. A row's tooltip shows below the row; a wheel notch moves that row 60px, so with the
// pointer still the old row's tooltip slid under the pointer and took the next wheel events.
// Make one as a child of the view (it draws and covers nothing) and add `&& gate.open` to each row's `ToolTip.visible`: the
// tooltips are gated shut when the content moves and open again when the pointer moves.
Item {
    id: gate
    required property Flickable view
    // False from a scroll until the pointer moves.
    readonly property bool open: !shut

    property bool shut: false
    property real restX: -1
    property real restY: -1

    visible: false

    Connections {
        target: gate.view
        function onContentYChanged() {
            gate.restX = hover.point.scenePosition.x
            gate.restY = hover.point.scenePosition.y
            gate.shut = true
            gate.ToolTip.toolTip.close()
        }
    }
    HoverHandler {
        id: hover
        parent: gate.view // watches the whole view; nothing is laid over the rows
        blocking: false // it only watches: the rows under it keep their own hover
        onPointChanged: {
            if (point.scenePosition.x !== gate.restX || point.scenePosition.y !== gate.restY)
                gate.shut = false
        }
    }
}
