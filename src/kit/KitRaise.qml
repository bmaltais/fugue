import QtQuick
import Fugue

// The raised look of a clickable thing (docs/canvas-look.md "Drift kit", the rule: clickable things are raised, fillable things
// are recessed): a 1px light edge along the top (white at 7% on a dark theme, 60% on a light one) and a soft 1px shadow under it.
// Put it in a chip, a rail item or the Run pill, filling it, with the same `radius`; it takes no input. A frosted control has its
// own glass and does not use it.
Item {
    id: raise
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property real radius: 0
    anchors.fill: parent
    enabled: false
    Rectangle { // the soft shadow under the control
        anchors { fill: parent; topMargin: 1; bottomMargin: -1 }
        radius: raise.radius
        color: Qt.rgba(0, 0, 0, raise.look.light ? 0.10 : 0.28)
        z: -1
    }
    Rectangle { // the light top edge
        x: raise.radius * 0.6
        y: 0
        width: parent.width - 2 * x
        height: 1
        color: raise.look.light ? Qt.rgba(1, 1, 1, 0.6) : Qt.rgba(1, 1, 1, 0.07)
    }
}
