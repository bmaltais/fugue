import QtQuick

// A port's mark (spec 12.6, docs/canvas-look.md 5): color by type, and a shape so color is
// not the only signal. circle: a value type (text, number, boolean); square: media; diamond:
// a declared type; ring: `any`. Filled when wired, hollow when not; a list port has a second
// outline behind it, like a stack. A filled mark has a 2 px ring of the node's body color
// around its 10 px, which keeps it apart from the node's edge and the wire; a hollow one (no
// wire to keep apart from) is the 10 px mark alone. Place it by its center. Two items (the
// list outline hidden but for list ports), as the canvas makes one per port in view; the
// valid-target ring is drawn once by the canvas, not per port.
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
    readonly property bool solid: wired && shape !== "ring"
    readonly property real markRadius: round ? 5 : Math.min(corner, 2)
    readonly property int inset: solid ? 2 : 0 // the body ring around a filled mark

    width: 10 + 2 * inset
    height: width
    radius: round ? width / 2 : markRadius + inset
    rotation: shape === "diamond" ? 45 : 0
    scale: (shape === "diamond" ? 0.8 : 1) * (hot ? 1.4 : hovered ? 1.25 : 1)
    color: solid ? fill : body
    border.width: 2
    border.color: solid ? body : fill

    // A list port's second outline, offset up and right, at 70%.
    Rectangle {
        visible: dot.list
        z: -1
        x: dot.inset + (dot.shape === "diamond" ? 4.24 : 3)
        y: dot.inset + (dot.shape === "diamond" ? 0 : -3)
        width: 10
        height: 10
        radius: dot.markRadius
        color: "transparent"
        opacity: 0.7
        border.width: 1.5
        border.color: dot.fill
    }
}
