import QtQuick

// A port's mark (spec 12.6, docs/canvas-look.md 5): color by type, and a shape so color is
// not the only signal. circle: a value type (text, number, boolean); square: media; diamond:
// a declared type; ring: `any`. Filled when wired, hollow when not; a list port has a second
// outline behind it, like a stack. The item is a 2 px ring of the node's body color around the
// 10 px mark, which keeps it apart from the node's edge and the wire; place it by its center.
// The valid-target ring is drawn once by the canvas, not per port.
Rectangle {
    id: dot
    property string shape: "circle"
    property color fill: "white"
    property color body: "black" // the node's fill, for the separating ring and the hollow
    property bool list: false
    property bool wired: true
    property bool hot: false     // a valid target under a dragged link
    property bool hovered: false
    property int corner: 0       // the node corner r; squares round to min(r, 2)

    readonly property bool round: shape === "circle" || shape === "ring"
    readonly property real markRadius: round ? 5 : Math.min(corner, 2)

    width: 14
    height: 14
    radius: round ? 7 : markRadius + 2
    rotation: shape === "diamond" ? 45 : 0
    scale: (shape === "diamond" ? 0.8 : 1) * (hot ? 1.4 : hovered ? 1.25 : 1)
    color: body

    // A list port's second outline, offset up and right, at 70%.
    Rectangle {
        visible: dot.list
        z: -1
        x: dot.shape === "diamond" ? 2 + 4.24 : 5
        y: dot.shape === "diamond" ? 2 : -1
        width: 10
        height: 10
        radius: dot.markRadius
        color: "transparent"
        opacity: 0.7
        border.width: 1.5
        border.color: dot.fill
    }
    Rectangle {
        x: 2
        y: 2
        width: 10
        height: 10
        radius: dot.markRadius
        color: dot.wired && dot.shape !== "ring" ? dot.fill : dot.body
        border.width: dot.wired && dot.shape !== "ring" ? 0 : 2
        border.color: dot.fill
    }
}
