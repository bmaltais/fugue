import QtQuick
import Fugue

// The rounded card that holds a section's rows (docs/canvas-look.md "Drift kit"): the `kitGroup` fill a step off the
// panel it sits on, the popup corner, and its children stacked with no gap. A KitRow draws the hairline above itself
// when it is not the first visible child, so rows can come and go without touching the lines. Give it a width (a
// Column's, or anchors); its height is its rows'.
Rectangle {
    id: group
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    default property alias rows: column.data
    width: parent ? parent.width : implicitWidth
    implicitHeight: column.implicitHeight
    height: implicitHeight
    radius: look.popupCorner
    color: look.kitGroup
    Column {
        id: column
        width: parent.width
    }
}
