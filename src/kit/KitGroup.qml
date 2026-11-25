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
    // Over a tinted panel (Colour above 0) the fill is translucent, so the tint shows through; plain, it is the opaque `kitGroup`.
    readonly property bool glass: look.params.panelTint > 0.001
    radius: Math.min(look.params.cornerGroup * look.unit, 0.3 * height)
    color: glass ? (look.light ? Qt.rgba(0, 0, 0, look.params.groupFillAlphaLight) : Qt.rgba(1, 1, 1, look.params.groupFillAlpha)) : look.kitGroup
    border.width: look.params.borders === "hairline" && glass ? 1 : 0
    border.color: look.light ? Qt.rgba(0, 0, 0, 0.07) : Qt.rgba(1, 1, 1, 0.06)
    Column {
        id: column
        width: parent.width
    }
}
