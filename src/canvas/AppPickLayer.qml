pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// Click-to-expose for the app builder (#393, spec 12.19): while `model.appBuilder.picking` this layer sits over the canvas. A click on
// an input row adds that input to the app (a second click takes it out again), a click on a Save or Preview node adds its result
// as an output, and the row or node under the pointer is outlined first, so a click does what the outline shows. What the app
// already has is outlined softly in the accent. The pan and the zoom of the canvas are its own: the wheel and the middle button
// pass through to the canvas under, and Escape ends picking (the dialog comes back).
Item {
    id: pick
    required property CanvasLook look
    required property CanvasModel model
    property real panX: 0
    property real panY: 0
    property real zoom: 1
    readonly property AppBuilder builder: model.appBuilder
    readonly property bool active: builder.picking
    // The row or node a click would take now, in the world: {x, y, width, height}, or empty.
    property var hovered: null

    visible: active
    focus: active
    onActiveChanged: {
        hovered = null
        if (active)
            forceActiveFocus()
    }
    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape
    Keys.onEscapePressed: pick.builder.finishPicking()

    function worldOf(px, py) { return Qt.point((px - panX) / zoom, (py - panY) / zoom) }
    function aim(px, py) {
        const w = worldOf(px, py)
        const row = model.inputRowAt(w.x, w.y)
        let rect = row.node !== undefined ? model.inputRowRect(row.node, row.input) : Qt.rect(0, 0, 0, 0)
        if (row.node === undefined) {
            const node = model.nodeAt(w.x, w.y)
            if (node !== "")
                rect = model.nodeRect(node)
        }
        hovered = rect.width > 0 ? {x: rect.x, y: rect.y, width: rect.width, height: rect.height} : null
    }

    // What the app has: soft outlines, the outputs a little stronger.
    Item {
        x: pick.panX
        y: pick.panY
        scale: pick.zoom
        transformOrigin: Item.TopLeft
        Repeater {
            model: pick.builder.marks
            delegate: Rectangle {
                id: mark
                required property var modelData
                x: modelData.x
                y: modelData.y
                width: modelData.width
                height: modelData.height
                radius: pick.look.smallCorner
                color: Qt.alpha(pick.look.accent, modelData.output ? 0.10 : 0.16)
                border.width: 2 / pick.zoom
                border.color: Qt.alpha(pick.look.accent, 0.75)
            }
        }
        Rectangle {
            visible: pick.hovered !== null
            x: pick.hovered ? pick.hovered.x : 0
            y: pick.hovered ? pick.hovered.y : 0
            width: pick.hovered ? pick.hovered.width : 0
            height: pick.hovered ? pick.hovered.height : 0
            radius: pick.look.smallCorner
            color: "transparent"
            border.width: 2 / pick.zoom
            border.color: pick.look.text
        }
    }

    MouseArea {
        objectName: "appPickArea"
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        cursorShape: pick.hovered ? Qt.PointingHandCursor : Qt.ArrowCursor
        onPositionChanged: mouse => pick.aim(mouse.x, mouse.y)
        onExited: pick.hovered = null
        onClicked: mouse => {
            const w = pick.worldOf(mouse.x, mouse.y)
            pick.builder.pickAt(w.x, w.y)
        }
    }
}
