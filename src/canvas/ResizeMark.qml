import QtQuick
import QtQuick.Shapes

// The mark of a node's resize corner (#63, docs/canvas-look.md 4.1): a short stroke 0.75u
// inside the node's edge, concentric with its corner, so an L on square corners and an arc
// with short legs on round ones. Drawn for the bottom right corner and mirrored for the rest;
// the node places it in its corner. Sizes follow the unit u (theme.look.unit, 4 px at 12).
Shape {
    id: mark
    required property string corner // "nw", "ne", "sw" or "se"
    required property real nodeCorner
    required property color color
    required property real unit
    readonly property real inset: 0.75 * unit
    readonly property real arc: Math.max(0, nodeCorner - inset)
    readonly property real leg: Math.max(inset, 2.25 * unit - arc)
    readonly property real end: arc + leg // where the stroke ends, from the inset corner

    width: end + inset
    height: end + inset
    preferredRendererType: Shape.CurveRenderer
    transform: Scale {
        origin.x: mark.width / 2
        origin.y: mark.height / 2
        xScale: mark.corner.endsWith("w") ? -1 : 1
        yScale: mark.corner.startsWith("n") ? -1 : 1
    }

    ShapePath {
        strokeColor: mark.color
        strokeWidth: 0.375 * mark.unit
        fillColor: "transparent"
        capStyle: ShapePath.RoundCap
        startX: 0
        startY: mark.end
        PathLine { x: mark.leg; y: mark.end }
        PathArc {
            x: mark.end
            y: mark.leg
            radiusX: mark.arc
            radiusY: mark.arc
            direction: PathArc.Counterclockwise
        }
        PathLine { x: mark.end; y: 0 }
    }
}
