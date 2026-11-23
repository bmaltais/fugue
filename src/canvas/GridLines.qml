pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// The grid over a picture (#253): a line every `spacing` pixels of the picture, in both directions, following the
// viewer's zoom and pan. It draws only the lines the item shows, only on the picture, and not at all while the lines
// would be closer than `minGap` view units (a haze; `drawn` says so, and the snap leaves the grid alone then). It never
// takes the pointer.
//
// Input:  look, imageSize, originX, originY, zoom, spacing.
// Output: drawn.
Item {
    id: grid
    required property CanvasLook look
    property size imageSize: Qt.size(0, 0)
    property real originX: 0
    property real originY: 0
    property real zoom: 1
    property int spacing: 64
    property real minGap: 6
    readonly property bool drawn: spacing > 0 && spacing * zoom >= minGap && imageSize.width > 0 && imageSize.height > 0
    readonly property color lineColor: Qt.rgba(0.5, 0.5, 0.5, 0.5)
    // The first and last line of an axis in view, as indices (line n is at n * spacing).
    function firstLine(origin) { return Math.max(0, Math.ceil(-origin / zoom / spacing - 1e-9)) }
    function lastLine(origin, view, extent) { return Math.min(Math.floor(extent / spacing + 1e-9), Math.floor((view - origin) / zoom / spacing + 1e-9)) }
    readonly property int firstX: firstLine(originX)
    readonly property int lastX: lastLine(originX, width, imageSize.width)
    readonly property int firstY: firstLine(originY)
    readonly property int lastY: lastLine(originY, height, imageSize.height)
    clip: true
    enabled: false // never takes the pointer

    Repeater {
        model: grid.drawn && grid.lastX >= grid.firstX ? Math.min(2000, grid.lastX - grid.firstX + 1) : 0
        delegate: Rectangle {
            required property int index
            objectName: "gridColumn"
            x: Math.round(grid.originX + (grid.firstX + index) * grid.spacing * grid.zoom)
            y: Math.max(0, grid.originY)
            width: 1
            height: Math.min(grid.height, grid.originY + grid.imageSize.height * grid.zoom) - y
            color: grid.lineColor
        }
    }
    Repeater {
        model: grid.drawn && grid.lastY >= grid.firstY ? Math.min(2000, grid.lastY - grid.firstY + 1) : 0
        delegate: Rectangle {
            required property int index
            objectName: "gridRow"
            x: Math.max(0, grid.originX)
            y: Math.round(grid.originY + (grid.firstY + index) * grid.spacing * grid.zoom)
            width: Math.min(grid.width, grid.originX + grid.imageSize.width * grid.zoom) - x
            height: 1
            color: grid.lineColor
        }
    }
}
