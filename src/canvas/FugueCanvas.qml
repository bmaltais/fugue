pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes

// The canvas (spec 12): nodes as items and wires as one Shape each, under one item that is
// moved and scaled to pan and zoom (12.2, spike 05). The model is a CanvasModel
// (canvas_model.h); it owns the document, the geometry and every check, so this file only
// draws and turns pointer and key input into calls on it.
//
// One MouseArea under the world takes every press, hit-testing ports and nodes through the
// model, so the node delegates carry no input handlers. Navigation follows 12.3: the legacy
// preset (ComfyUI's default) pans with a left-drag on empty canvas and zooms with the wheel;
// the standard preset selects with a left-drag and pans with the wheel. In both, middle-drag
// and Space+drag pan, a trackpad (a wheel event with pixelDelta) pans and a pinch zooms, and
// right-drag does nothing.
//
// Editing (12.4): Shift or Ctrl+click adds a node to the selection, or takes one already in
// it out; Alt+drag clones what it drags and Alt+click takes the node out of the selection;
// Ctrl+Alt+click on a port disconnects it; Shift+drag from an output moves all its links.
// Each edit is one step on the model's undo stack. The keys (copy, paste, delete, undo...)
// are commands (canvas_commands.h); zoom and fit commands arrive as the model's
// zoomRequested and fitRequested.
FocusScope {
    id: canvas

    required property var model
    // "legacy" or "standard" (12.3). A setting once settings exist.
    property string navigation: "legacy"
    readonly property real minZoom: 0.1
    readonly property real maxZoom: 4
    readonly property alias zoom: world.scale
    readonly property alias panX: world.x
    readonly property alias panY: world.y

    // For node search (#18): a double-click on empty canvas, and a link let go on empty
    // canvas (`from` is the dragged port: {node, port, output}), at a world point.
    signal searchRequested(real x, real y)
    signal linkReleased(var from, real x, real y)

    property bool spaceHeld: false
    // "pan", "move", "link", "marquee", or "alt" (a press with Alt on a node, not yet a drag)
    // while a button is down
    property string gesture: ""
    property point pressAt        // where the gesture began, in canvas coordinates
    property point panStart       // world.x, world.y when a pan began
    property point pressWorld     // the press in world coordinates
    property point pointer        // the pointer in canvas coordinates during a gesture
    property bool travelled: false
    property string pressedNode: ""
    property bool pressedWasSelected: false
    property int pressModifiers: 0
    property string hoverProblem: ""
    readonly property var linkDrag: model.linkDrag
    // Below this zoom node text is too small to read and is not drawn.
    readonly property real readableZoom: 0.5

    // The world area on screen, and the padded area nodes draw their detail in. The padded
    // one moves only when the view leaves it, so a pan does not wake every node each frame.
    readonly property rect viewArea: Qt.rect(-world.x / world.scale, -world.y / world.scale,
                                             width / world.scale, height / world.scale)
    property rect detailArea
    function updateDetailArea() {
        const v = viewArea, d = detailArea
        if (v.x >= d.x && v.y >= d.y && v.x + v.width <= d.x + d.width && v.y + v.height <= d.y + d.height
                && d.width <= v.width * 3)
            return
        const padX = v.width / 2, padY = v.height / 2
        detailArea = Qt.rect(v.x - padX, v.y - padY, v.width + 2 * padX, v.height + 2 * padY)
    }
    onViewAreaChanged: updateDetailArea()

    function toWorld(px, py) {
        return Qt.point((px - world.x) / world.scale, (py - world.y) / world.scale)
    }
    // Where the pointer is, in world coordinates; the middle of the view when it is outside.
    // The `node.search` command (Tab, Shift+A) opens search here.
    function pointerWorld() {
        const p = hover.hovered ? hover.point.position : Qt.point(width / 2, height / 2)
        return toWorld(p.x, p.y)
    }
    HoverHandler { id: hover }
    // A paste lands at the pointer, wherever the view has moved since it stopped.
    Binding {
        target: canvas.model
        property: "pointer"
        value: hover.hovered ? canvas.toWorld(hover.point.position.x, hover.point.position.y) : undefined
    }
    function zoomAt(px, py, factor) {
        const s = Math.max(minZoom, Math.min(maxZoom, world.scale * factor))
        const w = toWorld(px, py)
        world.scale = s
        world.x = px - w.x * s
        world.y = py - w.y * s
        rememberView()
    }
    function panBy(dx, dy) {
        world.x += dx
        world.y += dy
        rememberView()
    }
    function rememberView() {
        const c = toWorld(width / 2, height / 2)
        model.setView(c.x, c.y, world.scale)
    }
    function applyView() {
        const v = model.view
        world.scale = v.zoom !== undefined ? v.zoom : 1
        const c = v.center !== undefined ? v.center : [width / 2, height / 2]
        world.x = width / 2 - c[0] * world.scale
        world.y = height / 2 - c[1] * world.scale
    }
    // The theme as nodes need it (theme.h's names), worked out once: its maps would be
    // converted again on every read.
    readonly property var look: ({
        ports: theme.portColors,
        palette: theme.colors,
        text: theme.text,
        muted: theme.muted,
        background: theme.background,
        panel: theme.panel,
        header: Qt.tint(theme.panel, Qt.alpha(theme.text, 0.05)),
        border: theme.border,
        accent: theme.accent,
        error: theme.error,
        corner: Math.min(theme.radius, 8),
        textSize: theme.textSize,
        smallText: Math.round(theme.textSize * 0.9)
    })
    // A port color key from the model (CanvasModel::colorKey).
    function portColor(key) {
        if (key === "any")
            return look.text
        if (key === "unknown")
            return look.muted
        const c = look.ports[key]
        return c !== undefined ? c : look.ports.declared
    }
    // A layout color (4.5) is a theme role: a semantic name, else a palette name.
    function roleColor(role) {
        const semantic = { accent: look.accent, muted: look.muted, text: look.text, error: look.error,
                           border: look.border, panel: look.panel }
        if (semantic[role] !== undefined)
            return semantic[role]
        const c = look.palette[role]
        return c !== undefined ? c : look.muted
    }

    // Nodes follow the desktop text size.
    Binding { target: canvas.model; property: "textSize"; value: theme.textSize }

    // A port is hit within 10 pixels on screen, and never less than its own size. Zoomed out
    // past readableZoom ports are not drawn, so a press there is on the node.
    function portUnder(px, py) {
        if (world.scale < readableZoom)
            return ({})
        const w = toWorld(px, py)
        return model.portAt(w.x, w.y, Math.max(8, 10 / world.scale))
    }

    clip: true
    focus: true
    // A graph flows left to right in every language: positions are geometry the file keeps,
    // so the canvas is never mirrored (the window around it is).
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true
    Component.onCompleted: applyView()
    Connections {
        target: canvas.model
        function onDocumentReset() { canvas.applyView() }
        function onZoomRequested(factor) { canvas.zoomAt(canvas.width / 2, canvas.height / 2, factor) }
        function onFitRequested() {
            if (canvas.model.fitView(canvas.width, canvas.height))
                canvas.applyView()
        }
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Space) {
            // A Space press alone does nothing; held, a drag pans (12.3).
            if (!event.isAutoRepeat)
                canvas.spaceHeld = true
            event.accepted = true
        }
    }
    Keys.onReleased: event => {
        if (event.key === Qt.Key_Space) {
            if (!event.isAutoRepeat)
                canvas.spaceHeld = false
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        color: theme.background
    }

    MouseArea {
        id: input
        objectName: "canvasInput"
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        cursorShape: canvas.gesture === "pan" ? Qt.ClosedHandCursor
                   : canvas.spaceHeld ? Qt.OpenHandCursor : Qt.ArrowCursor

        onPressed: mouse => {
            // A field on the canvas (a widget being typed into) gives up its focus first: the
            // canvas is its focus scope, so forceActiveFocus alone would hand focus back to it.
            if (canvas.Window.activeFocusItem)
                canvas.Window.activeFocusItem.focus = false
            canvas.forceActiveFocus()
            canvas.gesture = ""
            if (mouse.button === Qt.RightButton)
                return // the context menu comes later; right-drag does nothing
            canvas.pressAt = Qt.point(mouse.x, mouse.y)
            canvas.pointer = canvas.pressAt
            canvas.pressWorld = canvas.toWorld(mouse.x, mouse.y)
            canvas.travelled = false
            canvas.pressModifiers = mouse.modifiers
            if (mouse.button === Qt.MiddleButton || canvas.spaceHeld) {
                canvas.startPan()
                return
            }
            const port = canvas.portUnder(mouse.x, mouse.y)
            if (port.node !== undefined) {
                if ((mouse.modifiers & Qt.ControlModifier) && (mouse.modifiers & Qt.AltModifier)) {
                    canvas.model.disconnectPort(port.node, port.port, port.output)
                    return
                }
                canvas.gesture = "link"
                canvas.hoverProblem = ""
                if (port.output && (mouse.modifiers & Qt.ShiftModifier))
                    canvas.model.beginLinksMove(port.node, port.port)
                else
                    canvas.model.beginLinkDrag(port.node, port.port, port.output)
                return
            }
            const hit = canvas.model.nodeAt(canvas.pressWorld.x, canvas.pressWorld.y)
            if (hit !== "") {
                canvas.pressedNode = hit
                canvas.pressedWasSelected = canvas.model.selectedIds().indexOf(hit) >= 0
                if ((mouse.modifiers & Qt.AltModifier) && !(mouse.modifiers & Qt.ControlModifier)) {
                    canvas.gesture = "alt" // a clone once it travels
                    return
                }
                if (mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier))
                    canvas.model.select([hit], "add")
                else if (!canvas.pressedWasSelected)
                    canvas.model.select([hit], "replace")
                canvas.gesture = "move"
                canvas.model.beginMove(hit)
                return
            }
            const marquee = canvas.navigation === "standard" || (mouse.modifiers & Qt.ControlModifier)
            if (marquee)
                canvas.gesture = "marquee"
            else
                canvas.startPan()
        }

        onPositionChanged: mouse => {
            const w = canvas.toWorld(mouse.x, mouse.y)
            canvas.pointer = Qt.point(mouse.x, mouse.y)
            if (!canvas.travelled && Math.hypot(mouse.x - canvas.pressAt.x, mouse.y - canvas.pressAt.y)
                    >= Application.styleHints.startDragDistance)
                canvas.travelled = true
            if (canvas.gesture === "alt" && canvas.travelled) {
                canvas.model.beginCloneMove(canvas.pressedNode)
                canvas.gesture = "move"
            }
            switch (canvas.gesture) {
            case "pan":
                world.x = canvas.panStart.x + mouse.x - canvas.pressAt.x
                world.y = canvas.panStart.y + mouse.y - canvas.pressAt.y
                break
            case "move":
                canvas.model.moveBy(w.x - canvas.pressWorld.x, w.y - canvas.pressWorld.y)
                break
            case "link": {
                const port = canvas.portUnder(mouse.x, mouse.y)
                canvas.hoverProblem = port.node !== undefined
                    ? canvas.model.dragProblem(port.node, port.port, port.output) : ""
                break
            }
            }
        }

        onReleased: mouse => canvas.finishGesture(mouse.x, mouse.y, mouse.modifiers)
        onCanceled: canvas.finishGesture(canvas.pointer.x, canvas.pointer.y, 0)

        onDoubleClicked: mouse => {
            if (mouse.button !== Qt.LeftButton)
                return
            const w = canvas.toWorld(mouse.x, mouse.y)
            if (canvas.model.nodeAt(w.x, w.y) === "" && canvas.portUnder(mouse.x, mouse.y).node === undefined)
                canvas.searchRequested(w.x, w.y)
        }

        onWheel: wheel => {
            const trackpad = wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0
            const zooms = (wheel.modifiers & Qt.ControlModifier)
                || (!trackpad && canvas.navigation === "legacy")
            if (zooms) {
                const steps = (trackpad ? wheel.pixelDelta.y / 50 : wheel.angleDelta.y / 120)
                canvas.zoomAt(wheel.x, wheel.y, Math.pow(1.1, steps))
            } else if (trackpad) {
                canvas.panBy(wheel.pixelDelta.x, wheel.pixelDelta.y)
            } else {
                canvas.panBy(wheel.angleDelta.x / 2, wheel.angleDelta.y / 2)
            }
        }
    }

    function startPan() {
        gesture = "pan"
        panStart = Qt.point(world.x, world.y)
    }

    function finishGesture(px, py, modifiers) {
        const g = gesture
        gesture = ""
        switch (g) {
        case "pan":
            // A left click on empty canvas, not a drag, clears the selection.
            if (!travelled && !spaceHeld && navigation === "legacy")
                model.select([], "replace")
            rememberView()
            break
        case "move":
            model.endMove()
            if (!travelled) {
                if (!(pressModifiers & (Qt.ShiftModifier | Qt.ControlModifier)))
                    model.select([pressedNode], "replace")
                else if (pressedWasSelected)
                    model.select([pressedNode], "subtract")
            }
            break
        case "alt":
            // Alt+click, not a drag: the node leaves the selection.
            model.select([pressedNode], "subtract")
            break
        case "link": {
            const from = { node: linkDrag.node, port: linkDrag.port, output: linkDrag.output }
            // Links being moved (Shift+drag) let go on nothing stay as they were: no search.
            const moving = linkDrag.targets !== undefined
            const port = portUnder(px, py)
            if (port.node !== undefined) {
                model.endLinkDrag(port.node, port.port, port.output)
            } else {
                model.endLinkDrag()
                const w = toWorld(px, py)
                if (travelled && !moving && model.nodeAt(w.x, w.y) === "")
                    linkReleased(from, w.x, w.y)
            }
            hoverProblem = ""
            break
        }
        case "marquee": {
            const a = toWorld(Math.min(pressAt.x, px), Math.min(pressAt.y, py))
            const b = toWorld(Math.max(pressAt.x, px), Math.max(pressAt.y, py))
            const mode = (modifiers & Qt.ShiftModifier) ? "add"
                       : (modifiers & Qt.AltModifier) ? "subtract" : "replace"
            model.select(travelled ? model.nodesIn(Qt.rect(a.x, a.y, b.x - a.x, b.y - a.y)) : [], mode)
            break
        }
        }
    }

    PinchHandler {
        // A trackpad pinch arrives as a native zoom gesture; two fingers on a touch screen
        // also pan.
        target: null
        onScaleChanged: delta => canvas.zoomAt(centroid.position.x, centroid.position.y, delta)
        onTranslationChanged: delta => canvas.panBy(delta.x, delta.y)
    }

    Item {
        id: world
        objectName: "world"
        transformOrigin: Item.TopLeft

        Repeater {
            model: canvas.model.frames
            delegate: Rectangle {
                required property var modelData
                x: modelData.x
                y: modelData.y
                width: modelData.width
                height: modelData.height
                radius: canvas.look.corner
                color: Qt.alpha(canvas.roleColor(modelData.color), 0.12)
                border.width: 1
                border.color: Qt.alpha(canvas.roleColor(modelData.color), 0.5)
                Text {
                    x: 10
                    y: 6
                    width: parent.width - 20
                    text: parent.modelData.title
                    elide: Text.ElideRight
                    color: canvas.look.text
                    font.pixelSize: Math.round(canvas.look.textSize * 1.2)
                    font.bold: true
                }
            }
        }

        Repeater {
            id: wires
            objectName: "wires"
            model: canvas.model.links
            delegate: Shape {
                id: wire
                required property string path
                required property string color
                ShapePath {
                    strokeWidth: 2.5
                    strokeColor: canvas.portColor(wire.color)
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    PathSvg { path: wire.path }
                }
            }
        }

        Repeater {
            id: nodes
            objectName: "nodes"
            model: canvas.model.nodes
            delegate: CanvasNode {
                canvasModel: canvas.model
                linkDrag: canvas.linkDrag
                headerHeight: canvas.model.headerHeight
                rowHeight: canvas.model.rowHeight
                look: canvas.look
                canvasView: canvas
                detailArea: canvas.detailArea
                readable: world.scale >= canvas.readableZoom
            }
        }

        // The link being dragged, from its port to the pointer.
        Shape {
            id: draggedLink
            visible: canvas.linkDrag.node !== undefined
            readonly property point end: canvas.toWorld(canvas.pointer.x, canvas.pointer.y)
            readonly property real bend: Math.max(40, Math.abs(end.x - (canvas.linkDrag.x || 0)) * 0.5)
                                         * (canvas.linkDrag.output ? 1 : -1)
            ShapePath {
                strokeWidth: 2.5
                strokeColor: canvas.portColor(canvas.linkDrag.color || "any")
                strokeStyle: ShapePath.DashLine
                fillColor: "transparent"
                startX: canvas.linkDrag.x || 0
                startY: canvas.linkDrag.y || 0
                PathCubic {
                    x: draggedLink.end.x
                    y: draggedLink.end.y
                    control1X: (canvas.linkDrag.x || 0) + draggedLink.bend
                    control1Y: canvas.linkDrag.y || 0
                    control2X: draggedLink.end.x - draggedLink.bend
                    control2Y: draggedLink.end.y
                }
            }
        }
    }

    // The marquee, in canvas coordinates.
    Rectangle {
        visible: canvas.gesture === "marquee" && canvas.travelled
        x: Math.min(canvas.pressAt.x, canvas.pointer.x)
        y: Math.min(canvas.pressAt.y, canvas.pointer.y)
        width: Math.abs(canvas.pointer.x - canvas.pressAt.x)
        height: Math.abs(canvas.pointer.y - canvas.pressAt.y)
        color: Qt.alpha(canvas.look.accent, 0.12)
        border.width: 1
        border.color: canvas.look.accent
    }

    // Why the port under a dragged link cannot take it (12.6).
    Rectangle {
        objectName: "linkProblem"
        visible: canvas.gesture === "link" && canvas.hoverProblem !== ""
        x: Math.min(canvas.pointer.x + 16, canvas.width - width - 8)
        y: Math.min(canvas.pointer.y + 16, canvas.height - height - 8)
        width: Math.min(problemText.implicitWidth + 16, 360)
        height: problemText.implicitHeight + 10
        radius: Math.min(canvas.look.corner, 6)
        color: canvas.look.panel
        border.width: 1
        border.color: canvas.look.error
        Text {
            id: problemText
            x: 8
            y: 5
            width: parent.width - 16
            text: canvas.hoverProblem
            wrapMode: Text.Wrap
            color: canvas.look.text
            font.pixelSize: canvas.look.textSize
        }
    }
}
