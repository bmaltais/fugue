import QtQuick
import Fugue

// The hairline between the rows of a KitGroup (docs/canvas-look.md "Drift kit"): 1px, inset 3.5u from each side, the
// `kitSeparator` color. KitRow draws its own above itself; this one is for a row of the group's that is not a KitRow.
Item {
    id: separator
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    implicitHeight: 1
    width: parent ? parent.width : 0
    height: 1
    Rectangle {
        x: 3.5 * separator.look.unit
        width: parent.width - 2 * x
        height: 1
        color: separator.look.kitSeparator
    }
}
