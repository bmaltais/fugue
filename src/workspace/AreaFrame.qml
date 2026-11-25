import QtQuick
import QtQuick.Shapes
import Fugue

// The outline of one rounded space (docs/canvas-look.md 20.1 principle 5, 20.21): laid over a card's rectangle, it paints the four
// corners' outside in the window's ground colour, so the card's content (the canvas, a view, a header) takes the card's corner
// without a clipped layer of its own, and draws the 1px hairline round it. It takes no input. In the Panes layout (`areaLayout`)
// there are no corners and no outline: the borders between areas are the handles' 1px dividers.
Shape {
    id: frame
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property int areaId: 0
    // The active area's outline takes the accent; with one area there is no active one to tell.
    readonly property bool active: areaId > 0 && App.workspace !== null && App.workspace.count > 1 && App.workspace.active === areaId
    property color ground: App.theme.background // what shows between the cards: the window
    readonly property bool cards: look.params.areaLayout !== "panes"
    readonly property real r: Math.max(0, Math.min(look.params.cornerCard * look.unit, 0.2 * Math.min(width, height)))
    // Its size is the card's, set by the host: a size of its own (a Shape's default is its paths' extent, which here depends on it).
    width: 1
    height: 1
    property bool shown: true
    visible: shown && cards && width > 2 * r && height > 2 * r
    preferredRendererType: Shape.CurveRenderer
    antialiasing: true
    containsMode: Shape.BoundingRectContains
    enabled: false

    // The ground outside the rounded rectangle: the whole rectangle (a wide margin round it, against a seam), the rounded one cut out.
    ShapePath {
        strokeColor: "transparent"
        fillColor: frame.ground
        fillRule: ShapePath.OddEvenFill
        startX: -1
        startY: -1
        PathLine { x: frame.width + 1; y: -1 }
        PathLine { x: frame.width + 1; y: frame.height + 1 }
        PathLine { x: -1; y: frame.height + 1 }
        PathLine { x: -1; y: -1 }
        PathMove { x: frame.r; y: 0 }
        PathLine { x: frame.width - frame.r; y: 0 }
        PathArc { x: frame.width; y: frame.r; radiusX: frame.r; radiusY: frame.r }
        PathLine { x: frame.width; y: frame.height - frame.r }
        PathArc { x: frame.width - frame.r; y: frame.height; radiusX: frame.r; radiusY: frame.r }
        PathLine { x: frame.r; y: frame.height }
        PathArc { x: 0; y: frame.height - frame.r; radiusX: frame.r; radiusY: frame.r }
        PathLine { x: 0; y: frame.r }
        PathArc { x: frame.r; y: 0; radiusX: frame.r; radiusY: frame.r }
    }
    // The hairline, on the card's edge (half a pixel in, so the stroke is not clipped).
    ShapePath {
        strokeColor: frame.active ? Qt.alpha(frame.look.accent, 0.55)
                     : frame.look.params.borders === "hairline" ? (frame.look.light ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(1, 1, 1, 0.09))
                     : "transparent"
        strokeWidth: 1
        fillColor: "transparent"
        startX: frame.r
        startY: 0.5
        PathLine { x: frame.width - frame.r; y: 0.5 }
        PathArc { x: frame.width - 0.5; y: frame.r; radiusX: frame.r - 0.5; radiusY: frame.r - 0.5 }
        PathLine { x: frame.width - 0.5; y: frame.height - frame.r }
        PathArc { x: frame.width - frame.r; y: frame.height - 0.5; radiusX: frame.r - 0.5; radiusY: frame.r - 0.5 }
        PathLine { x: frame.r; y: frame.height - 0.5 }
        PathArc { x: 0.5; y: frame.height - frame.r; radiusX: frame.r - 0.5; radiusY: frame.r - 0.5 }
        PathLine { x: 0.5; y: frame.r }
        PathArc { x: frame.r; y: 0.5; radiusX: frame.r - 0.5; radiusY: frame.r - 0.5 }
    }
}
