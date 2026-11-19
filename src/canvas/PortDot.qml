import QtQuick

// A port's mark (spec 12.6): color by type, and a shape so color is not the only signal.
// circle: a value type (text, number, boolean); square: media; diamond: a declared type;
// ring: `any`. A list port has a second outline behind it, like a stack.
Rectangle {
    id: dot
    property string shape: "circle"
    property color fill: "white"
    property color edge: "black"
    property bool list: false

    width: 10
    height: 10
    radius: shape === "circle" || shape === "ring" ? width / 2 : 1
    rotation: shape === "diamond" ? 45 : 0
    scale: shape === "diamond" ? 0.8 : 1
    color: shape === "ring" ? "transparent" : fill
    border.width: shape === "ring" ? 2 : 1
    border.color: shape === "ring" ? fill : edge

    Rectangle {
        visible: dot.list
        z: -1
        x: 3
        y: -3
        width: dot.width
        height: dot.height
        radius: dot.radius
        color: "transparent"
        border.width: 1
        border.color: dot.fill
    }
}
