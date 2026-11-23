pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// The box of a layer being moved or transformed (#236, epic #216), drawn over the picture: the layer's original at its placement (a live
// preview, the canvas under it has the layer left out), an outline, and, when `handles` is on, Photoshop's Free Transform
// controls: eight handles (a corner or an edge scales from the opposite side), the body (drag to move) and a ring outside each corner
// (drag to turn). Shift keeps the proportions (and turns by 15 degrees, moves along one axis), Alt works from the center. The
// numbers are TransformGesture's (canvas/transform_math.h), tested apart; this item only turns pointer events into
// `changeRequested(placement)` and says when a drag starts and ends.
//
// Input:  look, gesture (a TransformGesture), placement (a placement map, canvas pixels), imageSource and opacity (the original's
//         picture), originX, originY, zoom (the viewer's pan and zoom, as CropFrame), handles, moveAnywhere (the Move tool: a press
//         anywhere on the picture moves the layer).
// Output: dragStarted(), changeRequested(placement) at every step, dragEnded(committed) (false after cancel()). `cancel()` ends
//         a drag as Escape does: the placement goes back to what it was when the drag began.
Item {
    id: frame
    required property CanvasLook look
    required property var gesture
    property var placement: null
    property url imageSource
    property real opacityOfLayer: 1
    property real originX: 0
    property real originY: 0
    property real zoom: 1
    property bool handles: false
    property bool moveAnywhere: false
    property bool yieldPress: false
    readonly property bool dragging: pointer.dragging
    readonly property real grab: 12 // screen px: a 24px press target on every handle (the floor)
    readonly property real handleSize: 8
    readonly property bool has: placement !== null && placement !== undefined && (placement.width ?? 0) > 0
    readonly property real k: zoom
    readonly property real boxW: has ? Math.round(placement.width) * k : 0
    readonly property real boxH: has ? Math.round(placement.height) * k : 0
    clip: true

    signal aboutToPress
    signal dragStarted
    signal changeRequested(var placement)
    signal dragEnded(bool committed)

    function cancel() { pointer.cancel() }
    // A canvas point from a point of this item.
    function canvasPoint(x, y) { return {x: (x - frame.originX) / frame.k, y: (y - frame.originY) / frame.k} }

    // The box: its center on the picture, turned about it. Everything of the box is inside this one item, so turning it turns all.
    Item {
        id: box
        visible: frame.has
        x: frame.originX + (frame.has ? frame.placement.cx : 0) * frame.k - width / 2
        y: frame.originY + (frame.has ? frame.placement.cy : 0) * frame.k - height / 2
        width: frame.boxW
        height: frame.boxH
        rotation: frame.has ? frame.placement.angle : 0
        transformOrigin: Item.Center
        Image {
            objectName: "transformPreview"
            anchors.fill: parent
            source: frame.imageSource
            opacity: frame.opacityOfLayer
            fillMode: Image.Stretch
            smooth: frame.has && frame.placement.filter !== "nearest"
            mipmap: true
            asynchronous: false
            cache: true
            transform: Scale {
                origin.x: box.width / 2
                origin.y: box.height / 2
                xScale: frame.has && frame.placement.flipX ? -1 : 1
                yScale: frame.has && frame.placement.flipY ? -1 : 1
            }
        }
        Rectangle {
            objectName: "transformOutline"
            anchors.fill: parent
            color: "transparent"
            border.width: 1
            border.color: frame.look.accent
        }
        // The rings outside the corners: a press there turns the layer.
        Repeater {
            model: frame.handles && frame.has ? [{u: 0, v: 0}, {u: 1, v: 0}, {u: 1, v: 1}, {u: 0, v: 1}] : []
            delegate: Item {
                id: ring
                required property var modelData
                objectName: "transformRotate-" + modelData.u + modelData.v
                x: box.width * modelData.u - (modelData.u === 0 ? 2 * frame.grab : 0)
                y: box.height * modelData.v - (modelData.v === 0 ? 2 * frame.grab : 0)
                width: 2 * frame.grab
                height: 2 * frame.grab
                MouseArea {
                    anchors.fill: parent
                    enabled: !frame.yieldPress
                    acceptedButtons: Qt.LeftButton
                    cursorShape: Qt.CrossCursor
                    onPressed: mouse => pointer.begin("rotate", mapToItem(frame, mouse.x, mouse.y))
                    onPositionChanged: mouse => pointer.move(mapToItem(frame, mouse.x, mouse.y), mouse.modifiers)
                    onReleased: pointer.finish(true)
                    onCanceled: pointer.finish(false)
                }
            }
        }
        // The body: a press inside the box moves the layer.
        MouseArea {
            objectName: "transformBody"
            anchors.fill: parent
            enabled: frame.handles && !frame.yieldPress
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeAllCursor
            onPressed: mouse => pointer.begin("move", mapToItem(frame, mouse.x, mouse.y))
            onPositionChanged: mouse => pointer.move(mapToItem(frame, mouse.x, mouse.y), mouse.modifiers)
            onReleased: pointer.finish(true)
            onCanceled: pointer.finish(false)
        }
        // The eight handles.
        Repeater {
            model: frame.handles && frame.has ? [{h: "top-left", u: 0, v: 0}, {h: "top-right", u: 1, v: 0}, {h: "bottom-right", u: 1, v: 1}, {h: "bottom-left", u: 0, v: 1},
                                                  {h: "top", u: 0.5, v: 0}, {h: "right", u: 1, v: 0.5}, {h: "bottom", u: 0.5, v: 1}, {h: "left", u: 0, v: 0.5}] : []
            delegate: Item {
                id: slot
                required property var modelData
                objectName: "transformHandle-" + modelData.h
                visible: (modelData.u === 0.5 ? box.width : modelData.v === 0.5 ? box.height : 1e9) >= 3 * frame.handleSize
                x: box.width * modelData.u - frame.grab
                y: box.height * modelData.v - frame.grab
                width: 2 * frame.grab
                height: 2 * frame.grab
                Rectangle {
                    anchors.centerIn: parent
                    width: frame.handleSize
                    height: frame.handleSize
                    color: "white"
                    border.width: 1
                    border.color: Qt.rgba(0, 0, 0, 0.7)
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: !frame.yieldPress
                    acceptedButtons: Qt.LeftButton
                    cursorShape: slot.modelData.u === 0.5 ? Qt.SizeVerCursor : slot.modelData.v === 0.5 ? Qt.SizeHorCursor
                                 : (slot.modelData.u === slot.modelData.v ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor)
                    onPressed: mouse => pointer.begin(slot.modelData.h, mapToItem(frame, mouse.x, mouse.y))
                    onPositionChanged: mouse => pointer.move(mapToItem(frame, mouse.x, mouse.y), mouse.modifiers)
                    onReleased: pointer.finish(true)
                    onCanceled: pointer.finish(false)
                }
            }
        }
    }
    // The Move tool: a press anywhere moves the layer (Photoshop's Move with nothing to aim at).
    MouseArea {
        objectName: "transformMoveAnywhere"
        anchors.fill: parent
        enabled: frame.moveAnywhere && !frame.yieldPress
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeAllCursor
        onPressed: mouse => {
            frame.aboutToPress()
            pointer.begin("move", Qt.point(mouse.x, mouse.y))
        }
        onPositionChanged: mouse => pointer.move(Qt.point(mouse.x, mouse.y), mouse.modifiers)
        onReleased: pointer.finish(true)
        onCanceled: pointer.finish(false)
    }

    // One drag at a time. A drag is computed from the placement it began with and where the pointer is (never step by step).
    Item {
        id: pointer
        property bool dragging: false
        property string handle: ""
        property var start: null
        property var from: null
        function begin(name, at) {
            if (dragging || !frame.has)
                return
            handle = name
            start = frame.placement
            from = frame.canvasPoint(at.x, at.y)
            dragging = true
            frame.dragStarted()
        }
        function move(at, modifiers) {
            if (!dragging)
                return
            frame.changeRequested(frame.gesture.dragged(start, handle, from, frame.canvasPoint(at.x, at.y), (modifiers & Qt.ShiftModifier) !== 0, (modifiers & Qt.AltModifier) !== 0))
        }
        function finish(committed) {
            if (!dragging)
                return
            dragging = false
            if (!committed)
                frame.changeRequested(start)
            frame.dragEnded(committed)
        }
        function cancel() { finish(false) }
    }
}
