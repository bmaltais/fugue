import QtQuick

// The shadow under a raised surface (docs/canvas-look.md 7): one of theme.look's three
// pre-painted 9-slice textures, stretched around `target`, a sibling placed after it. Level 1
// is a node at rest, 2 a selected node and the pills, 3 popups; levels 1 and 2 have the node
// corner, 3 the popup corner.
BorderImage {
    required property Item target
    required property var look // theme.look
    property int level: 3

    readonly property int reach: look.shadowMargins[level - 1]
    readonly property int edge: reach + (level === 3 ? look.popupCorner : look.corner) + 1

    x: target.x - reach
    y: target.y - reach
    width: target.width + 2 * reach
    height: target.height + 2 * reach
    visible: target.visible
    source: level === 1 ? look.shadow1 : level === 2 ? look.shadow2 : look.shadow3
    border.left: edge
    border.top: edge
    border.right: edge
    border.bottom: edge
}
