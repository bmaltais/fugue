pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import Fugue

// The pointer of a painted mask (#172, #230): the brush's ring and the press, drag and release that paint, erase, draw a
// lasso or a rectangle, size the brush (Alt+drag), pan (the middle button, or a left drag with Space held) and zoom (the
// wheel). One piece of code for every surface that paints a mask: the Mask editor and the paint window (MaskEditor.qml)
// and the Canvas view's mask brush (CanvasView.qml), so a stroke is the same gesture everywhere. It knows nothing of
// what it paints: `target` is a MaskLayer or a PaintLayer (beginStroke, strokeTo, endStroke, fillPolygon, fillRect), and
// `viewer` the CompareView under it (pointAt, zoomAt, setView, imageScale).
//
// Input: look, viewer, target, imageSize (the picture's pixels, which the points are in), tool ("brush", "eraser",
// "lasso", "rect"), brushSize (picture pixels), brushHardness (0 to 1), spaceHeld, `flipErase` (the eraser paints and the
// brush erases: a layer mask, whose painted part is the hidden part), `prefix` (the objectNames: <prefix>Pointer,
// <prefix>BrushRing, <prefix>RectPreview). Output: `resize(size, hardness)` during an Alt+drag, `painted()` after each
// stroke or shape (a caller that batches strokes into one revision starts its idle there), `pressedAt()` on a press.
Item {
    id: pen
    required property CanvasLook look
    required property var viewer
    property var target: null
    property size imageSize: Qt.size(0, 0)
    property string tool: "brush"
    property real brushSize: 64
    property real brushHardness: 0.8
    property bool spaceHeld: false
    property bool flipErase: false
    property string prefix: "mask"
    signal resize(real size, real hardness)
    signal painted
    signal pressedAt

    readonly property bool erasing: tool === "eraser"
    readonly property bool brushing: tool === "brush" || tool === "eraser"
    readonly property alias area: mouse
    readonly property alias action: mouse.action

    // The lasso's outline and the rectangle, in view coordinates, while a drag is on.
    Shape {
        anchors.fill: parent
        visible: mouse.action === "lasso" && mouse.screenPath.length > 1
        ShapePath {
            strokeColor: Qt.rgba(0, 0, 0, 0.7)
            strokeWidth: 3
            fillColor: "transparent"
            PathPolyline { path: mouse.screenPath }
        }
        ShapePath {
            strokeColor: "white"
            strokeWidth: 1
            fillColor: Qt.alpha(pen.look.accent, 0.2)
            PathPolyline { path: mouse.screenPath }
        }
    }
    Rectangle {
        objectName: pen.prefix + "RectPreview"
        visible: mouse.action === "rect"
        x: Math.min(mouse.pressAt.x, mouse.at.x)
        y: Math.min(mouse.pressAt.y, mouse.at.y)
        width: Math.abs(mouse.at.x - mouse.pressAt.x)
        height: Math.abs(mouse.at.y - mouse.pressAt.y)
        color: Qt.alpha(pen.look.accent, 0.2)
        border.width: 1
        border.color: "white"
    }

    // The brush: its circle, and the hardness as an inner ring. Drawn dark under white so it reads on any picture.
    Item {
        id: ring
        objectName: pen.prefix + "BrushRing"
        readonly property bool shown: mouse.over && !pen.spaceHeld && pen.brushing && mouse.action !== "pan"
        readonly property real diameter: Math.max(2, pen.brushSize * pen.viewer.imageScale)
        readonly property point center: mouse.action === "resize" ? mouse.pressAt : mouse.at
        visible: shown
        x: center.x
        y: center.y
        component Circle: Rectangle {
            required property real across
            property color edge: "white"
            property real thickness: 1
            x: -across / 2
            y: -across / 2
            width: across
            height: across
            radius: across / 2
            color: "transparent"
            border.width: thickness
            border.color: edge
        }
        Circle { across: ring.diameter + 2; edge: Qt.rgba(0, 0, 0, 0.6); thickness: 1 }
        Circle { across: ring.diameter; edge: pen.erasing ? "#ff9f9a" : "white" }
        Circle {
            visible: pen.brushHardness < 0.97 && ring.diameter * pen.brushHardness > 6
            across: ring.diameter * pen.brushHardness
            edge: Qt.rgba(1, 1, 1, 0.55)
        }
        Rectangle { x: -1; y: -1; width: 3; height: 3; radius: 1.5; color: "white" }
    }

    MouseArea {
        id: mouse
        objectName: pen.prefix + "Pointer"
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton // the middle button pans
        preventStealing: true
        cursorShape: pen.spaceHeld ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
                     : pen.brushing ? Qt.BlankCursor : Qt.CrossCursor

        property bool over: containsMouse
        property point at: Qt.point(0, 0)        // the pointer, in view coordinates
        property point pressAt: Qt.point(0, 0)   // where the press was
        property point last: Qt.point(0, 0)
        property string action: ""               // stroke, lasso, rect, pan, resize ("" when no drag)
        property var path: []                    // the lasso's points, in picture pixels
        property var screenPath: []              // the same, in view coordinates
        property real startSize: 64
        property real startHardness: 0.8

        // A tablet's pressure scales the brush; a mouse is full.
        PointHandler {
            id: stylus
            acceptedDevices: PointerDevice.Stylus
        }
        readonly property real pressure: stylus.active ? Math.max(0.05, stylus.point.pressure) : 1

        // View coordinates to picture pixels.
        function pixelAt(x, y) {
            const f = pen.viewer.pointAt(x, y)
            return Qt.point(f.x * pen.imageSize.width, f.y * pen.imageSize.height)
        }
        // What a paint or erase does to the target: a layer mask's painted part is its hidden part (`flipErase`).
        function erases(alt) { return (pen.erasing || alt) !== pen.flipErase }

        onPressed: mouse => {
            pen.pressedAt()
            at = Qt.point(mouse.x, mouse.y)
            pressAt = at
            last = at
            const p = pixelAt(mouse.x, mouse.y)
            if (pen.spaceHeld || mouse.button === Qt.MiddleButton) {
                action = "pan"
            } else if ((mouse.modifiers & Qt.AltModifier) && pen.brushing) {
                action = "resize"
                startSize = pen.brushSize
                startHardness = pen.brushHardness
            } else if (!pen.target) {
                action = ""
            } else if (pen.brushing) {
                action = "stroke"
                pen.target.beginStroke(erases(false), pen.brushSize, pen.brushHardness, p.x, p.y, pressure)
            } else if (pen.tool === "lasso") {
                action = "lasso"
                path = [p]
                screenPath = [Qt.point(at.x, at.y)]
            } else {
                action = "rect"
            }
        }
        onPositionChanged: mouse => {
            at = Qt.point(mouse.x, mouse.y)
            if (!pressed)
                return
            const p = pixelAt(mouse.x, mouse.y)
            if (action === "stroke") {
                pen.target.strokeTo(p.x, p.y, pressure)
            } else if (action === "lasso") {
                const prior = screenPath[screenPath.length - 1]
                if (Math.abs(prior.x - at.x) + Math.abs(prior.y - at.y) >= 2) {
                    path = path.concat([p])
                    screenPath = screenPath.concat([Qt.point(at.x, at.y)])
                }
            } else if (action === "resize") {
                // Right grows and up hardens, as ComfyUI's Alt+drag does; the ring follows the pointer's edge.
                pen.resize(startSize + 2 * (at.x - pressAt.x) / pen.viewer.imageScale, startHardness + (pressAt.y - at.y) / 200)
            } else if (action === "pan") {
                if (!pen.viewer.actual && pen.viewer.zoom <= 1)
                    return // at Fit the picture stays put
                pen.viewer.setView({centerX: pen.viewer.centerX - (at.x - last.x) / (pen.viewer.aw * pen.viewer.imageScale),
                                    centerY: pen.viewer.centerY - (at.y - last.y) / (pen.viewer.ah * pen.viewer.imageScale)})
                last = at
            }
        }
        // The wheel zooms at the pointer, as the viewer's does (this area is over it).
        onWheel: wheel => pen.viewer.zoomAt(wheel.x, wheel.y, Math.pow(1.15, wheel.angleDelta.y / 120))
        onReleased: mouse => finish(mouse.modifiers)
        onCanceled: finish(Qt.NoModifier)
        onContainsMouseChanged: if (!containsMouse && !pressed) action = ""

        function finish(modifiers) {
            const alt = (modifiers & Qt.AltModifier) !== 0
            let done = false
            if (action === "stroke") {
                pen.target.endStroke()
                done = true
            } else if (action === "lasso") {
                if (path.length >= 3) {
                    pen.target.fillPolygon(path, erases(alt))
                    done = true
                }
            } else if (action === "rect") {
                const a = pixelAt(pressAt.x, pressAt.y), b = pixelAt(at.x, at.y)
                if (Math.abs(b.x - a.x) >= 1 && Math.abs(b.y - a.y) >= 1) {
                    pen.target.fillRect(Math.min(a.x, b.x), Math.min(a.y, b.y), Math.abs(b.x - a.x), Math.abs(b.y - a.y), erases(alt))
                    done = true
                }
            }
            action = ""
            path = []
            screenPath = []
            if (done)
                pen.painted()
        }
    }
}
