pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import Fugue

// The canvas (spec 12): nodes as items and wires as one Shape each, under one item that is
// moved and scaled to pan and zoom (12.2, spike 05). The model is a CanvasModel
// (canvas_model.h); it owns the document, the geometry and every check, so this file only
// draws and turns pointer and key input into calls on it.
//
// One MouseArea under the world takes every press, hit-testing ports and nodes through the
// model, so the node delegates carry no input handlers but three made only where they show: a
// node's warning badge (#72), a group card's Enter button and the in-place title editor (#74).
// Navigation follows 12.3: the legacy
// preset (ComfyUI's default) pans with a left-drag on empty canvas and zooms with the wheel;
// the standard preset selects with a left-drag and pans with the wheel. In both, middle-drag
// and Space+drag pan, a touchpad (a wheel event with a scroll phase;
// never told by pixelDelta, which a mouse wheel can carry too) pans and a pinch zooms, and
// right-drag does nothing.
//
// Editing (12.4): Shift or Ctrl+click adds a node to the selection, or takes one already in
// it out; Alt+drag clones what it drags and Alt+click takes the node out of the selection;
// Ctrl+Alt+click on a port disconnects it; Shift+drag from an output moves all its links.
// Each edit is one step on the model's undo stack. The keys (copy, paste, delete, undo...)
// are commands (canvas_commands.h); zoom and fit commands arrive as the model's
// zoomRequested and fitRequested.
//
// The look is docs/canvas-look.md, from theme.look (look.h). Layers, back to front: the dot
// grid (one shader), frames, comments, wires, the highlighted wires' casings, reroute points,
// node shadows (one shared 9-slice texture, its own Repeater so it batches), nodes, frame
// titles when zoomed out, the dragged link; then, in screen space, the cut handle, the
// marquee, the reason chip and the zoom pill. Detail follows the zoom tier (`tier`), never
// the zoom itself, so a wheel step wakes nothing until a tier changes.
FocusScope {
    id: canvas

    required property CanvasModel model
    // "legacy" or "standard" (12.3): settings.json's navigation, bound in Main.qml.
    property string navigation: "legacy"
    // The bars the window may hide (#129): the toolbar above a selection, the group breadcrumb and the
    // run and zoom pills.
    property bool selectionToolbarShown: true
    property bool breadcrumbShown: true
    property bool runPillShown: true
    property bool zoomPillShown: true
    readonly property real minZoom: 0.1
    readonly property real maxZoom: 4
    readonly property alias zoom: world.scale
    readonly property alias panX: world.x
    readonly property alias panY: world.y

    // For node search (#18): a double-click on empty canvas, and a link let go on empty
    // canvas (`from` is the dragged port: {node, port, output}), at a world point.
    signal searchRequested(real x, real y)
    signal linkReleased(var from, real x, real y)
    // A right-click, in canvas coordinates: the window opens the context menu (#50).
    signal contextMenuRequested(real x, real y)
    // A named menu (menus.h) for the selection at a canvas point: the selection toolbar's (#74).
    signal menuRequested(string kind, real x, real y)

    property bool spaceHeld: false
    // "pan", "move", "link", "marquee", or "alt" (a press with Alt on a node, not yet a drag)
    // while a button is down
    property string gesture: ""
    property int gestureButton: 0  // the button that holds the gesture
    property int ignoredButtons: 0 // pressed while another held a gesture: their release does nothing
    property point pressAt        // where the gesture began, in canvas coordinates
    property point rightPressAt   // where the right button went down
    property point panStart       // world.x, world.y when a pan began
    property point pressWorld     // the press in world coordinates
    property point pointer        // the pointer in canvas coordinates during a gesture
    property bool travelled: false
    property string pressedNode: ""
    property bool pressedWasSelected: false
    property int pressModifiers: 0
    property string hoverProblem: ""
    readonly property var linkDrag: model.linkDrag
    readonly property bool dragging: linkDrag.node !== undefined
    // Below this zoom node text is too small to read and is not drawn.
    readonly property real readableZoom: 0.5
    // The zoom tier (docs/canvas-look.md 14): 0 full detail, 1 the map (from half the
    // readable zoom), 2 the overview.
    readonly property int tier: world.scale >= readableZoom ? 0 : world.scale >= readableZoom / 2 ? 1 : 2

    // What the pointer is over, worked out by the model as it moves (no handlers per item).
    property string hoveredNode: ""
    property var hoverPort: ({})
    property var hoverLink: ({})
    // The resize corner the pointer is on, {node, corner} (#49, #63): any corner of an open
    // node, a square 4u wide (at least 12 screen px) inside it, unless a port is there too
    // (a port wins: a link drag from it); and the one being dragged.
    property var hoverCorner: ({})
    property string resizeCorner: ""
    function cornerAt(w) {
        if (tier !== 0 || model.portAt(w.x, w.y, Math.max(8, 10 / world.scale)).node !== undefined)
            return ({})
        return model.resizeCornerAt(w.x, w.y, Math.max(4 * model.unit, 12 / world.scale))
    }
    // The resize cursor of a corner: diagonal one way for nw and se, the other for ne and sw.
    function cornerCursor(corner) {
        return corner === "nw" || corner === "se" ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
    }
    // `wiresChanged`: the wires changed under a still pointer, so the hovered one is looked up
    // again even when its row number is the same (that row may hold another link now).
    function updateHover(px, py, wiresChanged) {
        followLens(px, py)
        const w = toWorld(px, py)
        const n = gesture === "" ? model.nodeAt(w.x, w.y) : ""
        if (n !== hoveredNode)
            hoveredNode = n
        const c = n !== "" ? cornerAt(w) : ({})
        if (c.node !== hoverCorner.node || c.corner !== hoverCorner.corner)
            hoverCorner = c
        const p = (tier <= 1 || inLens(px, py)) && (gesture === "" || gesture === "link")
            ? model.portAt(w.x, w.y, Math.max(8, 10 / scaleAt(px, py))) : ({})
        if (p.node !== hoverPort.node || p.port !== hoverPort.port || p.output !== hoverPort.output)
            hoverPort = p
        const l = tier === 0 && gesture === "" && n === "" && p.node === undefined && !model.linksHidden
            ? model.linkAt(w.x, w.y, 6 / world.scale) : ({})
        if (l.row !== hoverLink.row || (wiresChanged && (l.toNode !== hoverLink.toNode || l.toPort !== hoverLink.toPort
                || l.item !== hoverLink.item || l.fromNode !== hoverLink.fromNode || l.midX !== hoverLink.midX
                || l.midY !== hoverLink.midY))) {
            hoverLink = l
            refreshHighlights()
        }
    }
    function clearHover() {
        hoveredNode = ""
        hoverCorner = ({})
        hoverPort = ({})
        if (hoverLink.row !== undefined) {
            hoverLink = ({})
            refreshHighlights()
        }
    }

    // The wires drawn on top with a casing: the selection's and the hovered one (8.2). Worked
    // out once per event loop pass however many rows changed, and only for a selection of a
    // reasonable size: lighting every wire of a select-all shows nothing and costs a Shape each.
    readonly property int maxHighlights: 64
    property var highlightRows: []
    function refreshHighlights() { Qt.callLater(canvas.computeHighlights) }
    function computeHighlights() {
        let rows = model.linkRows(model.selection)
        if (rows.length > maxHighlights)
            rows = []
        if (hoverLink.row !== undefined && rows.indexOf(hoverLink.row) < 0)
            rows.push(hoverLink.row)
        if (rows.length !== highlightRows.length || rows.some((r, i) => r !== highlightRows[i]))
            highlightRows = rows
    }
    Connections {
        target: canvas.model
        function onSelectionChanged() { canvas.refreshHighlights() }
    }
    Connections {
        target: canvas.model.links
        function onRowsInserted() { canvas.rehover(); canvas.refreshHighlights() }
        function onRowsRemoved() { canvas.highlightRows = []; canvas.rehover(); canvas.refreshHighlights() }
        // A wire moved (one signal per wire): looked at once, after the batch.
        function onDataChanged() { if (canvas.gesture === "" && canvas.hoverLink.row !== undefined) Qt.callLater(canvas.rehover) }
        function onModelReset() { canvas.hoverLink = ({}); canvas.highlightRows = []; canvas.refreshHighlights() }
    }
    // An edit from a key (undo, delete) changed the wires: what the pointer is over is worked
    // out again now, so a cut handle never stays on a link that moved or went.
    function rehover() {
        if (gesture !== "")
            return
        if (hover.hovered)
            updateHover(hover.point.position.x, hover.point.position.y, true)
        else if (hoverLink.row !== undefined)
            clearHover()
    }

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
    onViewAreaChanged: {
        updateDetailArea()
        closeLens()
        if (dropChoice.node !== undefined)
            chooseDrop(null) // the list sits by the drop on screen
        closeWarnings() // the warnings card sits by its badge on screen: a pan or a zoom takes it away
    }

    function toWorld(px, py) {
        if (inLens(px, py))
            return lensWorld.mapFromItem(canvas, px, py) // the magnified node's world (#74)
        return Qt.point((px - world.x) / world.scale, (py - world.y) / world.scale)
    }
    // The zoom a point is seen at: the magnifier's inside it, else the canvas's.
    function scaleAt(px, py) { return inLens(px, py) ? lensWorld.scale : world.scale }

    // The magnifier (#74): far out (the map tier and beyond), a short hover over a node shows it
    // at zoom 1 over itself, its ports live, so a wire can start there; during a drag the node
    // under the pointer shows its own, to drop on. Inside it, toWorld() goes through it, so hover,
    // press and drop work as on the canvas. The view never moves; leaving it, a pan or a zoom, or
    // a press off its ports closes it.
    // settings.json's hover_magnifier (#123), bound in Main.qml: off unless turned on.
    property bool hoverMagnifier: false
    onHoverMagnifierChanged: {
        if (!hoverMagnifier) {
            lensTimer.stop()
            lensCandidate = ""
            closeLens()
        }
    }
    property string lensNode: ""
    property string lensCandidate: ""
    // A wire started in the magnifier: once it travels, that magnifier goes, so the canvas around
    // (and the next node's magnifier) can be reached.
    property bool lensSource: false
    readonly property bool lensOpen: lensNode !== ""
    function inLens(px, py) {
        return lensOpen && px >= lens.x && py >= lens.y && px < lens.x + lens.width && py < lens.y + lens.height
    }
    function openLens(node) {
        lensNode = node
        model.setLensNode(node)
    }
    function closeLens() {
        if (lensNode === "")
            return
        lensNode = ""
        model.setLensNode("")
    }
    // Called with the pointer: inside the magnifier it stays; elsewhere the node under it, far out,
    // is the next one to magnify after a short hover.
    function followLens(px, py) {
        if (gesture === "link" && lensSource && travelled) {
            lensSource = false
            closeLens()
        }
        if (inLens(px, py))
            return
        closeLens()
        if (!hoverMagnifier)
            return
        const w = Qt.point((px - world.x) / world.scale, (py - world.y) / world.scale)
        const n = tier >= 1 && (gesture === "" || gesture === "link") ? model.nodeAt(w.x, w.y) : ""
        if (n !== lensCandidate) {
            lensCandidate = n
            if (n !== "")
                lensTimer.restart()
            else
                lensTimer.stop()
        }
    }
    Timer {
        id: lensTimer
        interval: canvas.gesture === "link" ? 300 : 400
        onTriggered: {
            if (canvas.tier >= 1 && canvas.lensCandidate !== "" && canvas.lensCandidate !== canvas.lensNode)
                canvas.openLens(canvas.lensCandidate)
        }
    }
    onTierChanged: if (tier === 0) closeLens()
    // Where the pointer is, in world coordinates; the middle of the view when it is outside.
    // The `node.search` command (Tab, Shift+A) opens search here.
    function pointerWorld() {
        const p = hover.hovered ? hover.point.position : Qt.point(width / 2, height / 2)
        return toWorld(p.x, p.y)
    }
    HoverHandler {
        id: hover
        onPointChanged: if (canvas.gesture === "") canvas.updateHover(point.position.x, point.position.y)
        onHoveredChanged: if (!hovered) canvas.clearHover()
    }
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
    // The look (look.h), and the port colors as a JS object, converted once per theme change.
    readonly property CanvasLook look: App.theme.look
    // The window's runs (#19, run/canvas_runs.h), when the app gives them: a window made
    // without them (most tests) draws no run pill and no running dots.
    readonly property CanvasRuns windowRuns: App.runs
    readonly property var ports: App.theme.look.ports
    readonly property var shadowMargins: App.theme.look.shadowMargins
    // A port color key from the model (CanvasModel::colorKey).
    function portColor(key) {
        const c = ports[key]
        return c !== undefined ? c : ports.declared
    }
    // A wire's tangent length (8.1), the same rule as the model's.
    function bend(ax, bx) {
        const u = look.unit, run = bx - ax
        return run >= 6 * u ? Math.max(6 * u, Math.min(40 * u, run * 0.5)) : 12 * u + Math.min(Math.abs(run), 100 * u) * 0.25
    }

    // Nodes follow the desktop text size.
    Binding { target: canvas.model; property: "textSize"; value: App.theme.textSize }

    // A port is hit within 10 pixels on screen, and never less than its own size, wherever ports
    // are drawn: the map tier too (#74, as ComfyUI). In the overview a press is on the node.
    function portUnder(px, py) {
        if (tier > 1 && !inLens(px, py))
            return ({})
        const w = toWorld(px, py)
        return model.portAt(w.x, w.y, Math.max(8, 10 / scaleAt(px, py)))
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
        function onViewMoveRequested() { canvas.applyView() }
        function onZoomRequested(factor) { canvas.zoomAt(canvas.width / 2, canvas.height / 2, factor) }
        function onFitRequested() {
            if (canvas.model.fitView(canvas.width, canvas.height))
                canvas.applyView()
        }
    }

    // Esc closes the drop list or a pinned warnings card first, before the window's keys (Leave
    // group, Cancel run) see it.
    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape
                                                       && (dropChoice.node !== undefined || warningsNode !== "")
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape && canvas.dropChoice.node !== undefined) {
            canvas.chooseDrop(null)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Escape && canvas.warningsNode !== "") {
            canvas.closeWarnings()
            event.accepted = true
            return
        }
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
        color: canvas.look.canvas
    }
    // The dot grid (11): a dot every 6u in world space, a major one every fifth, in one draw.
    // Minor dots fade out between zoom 0.5 and 0.35. The software renderer has no shaders.
    Loader {
        anchors.fill: parent
        active: GraphicsInfo.api !== GraphicsInfo.Software
        sourceComponent: ShaderEffect {
            readonly property size size: Qt.size(width, height)
            readonly property point origin: Qt.point(world.x, world.y)
            readonly property real zoom: world.scale
            readonly property real spacing: 6 * canvas.look.unit
            readonly property real minorAlpha: Math.max(0, Math.min(1, (world.scale - 0.35) / 0.15))
            readonly property real minorRadius: 0.9
            readonly property real majorRadius: 1.2
            readonly property color dotColor: canvas.look.dot
            readonly property color majorColor: canvas.look.dotMajor
            fragmentShader: "qrc:/canvas/shaders/grid.frag.qsb"
        }
    }

    MouseArea {
        id: input
        objectName: "canvasInput"
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        cursorShape: canvas.gesture === "resize" ? canvas.cornerCursor(canvas.resizeCorner)
                   : canvas.gesture === "" && canvas.hoverCorner.corner !== undefined
                     ? canvas.cornerCursor(canvas.hoverCorner.corner)
                   : canvas.gesture === "pan" ? Qt.ClosedHandCursor
                   : canvas.spaceHeld ? Qt.OpenHandCursor : Qt.ArrowCursor

        onPressed: mouse => {
            canvas.closeWarnings() // a pinned warnings card goes at a click elsewhere
            if (canvas.dropChoice.node !== undefined)
                canvas.chooseDrop(null) // so does a wire waiting for its port
            // A second button while one holds a gesture is left out of it: the gesture goes
            // on, and that button's release does nothing (no context menu mid-drag).
            if (canvas.gesture !== "") {
                if (mouse.buttons & canvas.gestureButton) {
                    canvas.ignoredButtons |= mouse.button
                    return
                }
                canvas.finishGesture(canvas.pointer.x, canvas.pointer.y, 0) // its release never came
            }
            // A field on the canvas (a widget being typed into) gives up its focus first: the
            // canvas is its focus scope, so forceActiveFocus alone would hand focus back to it.
            if (canvas.Window.activeFocusItem)
                canvas.Window.activeFocusItem.focus = false
            canvas.forceActiveFocus()
            // In the magnifier only a port is taken (a wire starts); a press anywhere else closes
            // it and is a press on the canvas under it.
            if (canvas.inLens(mouse.x, mouse.y) && canvas.portUnder(mouse.x, mouse.y).node === undefined)
                canvas.closeLens()
            if (mouse.button === Qt.RightButton) {
                canvas.rightPressAt = Qt.point(mouse.x, mouse.y) // a click opens the menu on release
                return
            }
            canvas.gestureButton = mouse.button
            canvas.pressAt = Qt.point(mouse.x, mouse.y)
            canvas.pointer = canvas.pressAt
            canvas.pressWorld = canvas.toWorld(mouse.x, mouse.y)
            canvas.travelled = false
            canvas.pressModifiers = mouse.modifiers
            if (mouse.button === Qt.MiddleButton || canvas.spaceHeld) {
                canvas.startPan()
                return
            }
            // The hovered wire's cut handle removes its link (8.2).
            if (cutHandle.visible && Math.hypot(mouse.x - cutHandle.cx, mouse.y - cutHandle.cy) <= cutHandle.width / 2) {
                const l = canvas.hoverLink
                canvas.clearHover()
                canvas.model.disconnectInput(l.toNode, l.toPort, l.item)
                return
            }
            // A node's corner resizes it.
            const corner = mouse.button === Qt.LeftButton ? canvas.cornerAt(canvas.pressWorld) : ({})
            canvas.clearHover()
            if (corner.node !== undefined) {
                canvas.gesture = "resize"
                canvas.pressedNode = corner.node
                canvas.resizeCorner = corner.corner
                canvas.model.beginResize(corner.node, corner.corner)
                return
            }
            const port = canvas.portUnder(mouse.x, mouse.y)
            if (port.node !== undefined) {
                if ((mouse.modifiers & Qt.ControlModifier) && (mouse.modifiers & Qt.AltModifier)) {
                    canvas.model.disconnectPort(port.node, port.port, port.output)
                    return
                }
                canvas.gesture = "link"
                canvas.lensSource = canvas.inLens(mouse.x, mouse.y)
                canvas.hoverProblem = ""
                if (port.output && (mouse.modifiers & Qt.ShiftModifier))
                    canvas.model.beginLinksMove(port.node, port.port)
                else
                    canvas.model.beginLinkDrag(port.node, port.port, port.output)
                return
            }
            // The Advanced section's header opens and folds it (5.4, 6.1).
            if (mouse.button === Qt.LeftButton && canvas.tier === 0) {
                const folded = canvas.model.advancedHeaderAt(canvas.pressWorld.x, canvas.pressWorld.y)
                if (folded !== "") {
                    canvas.model.toggleAdvanced(folded)
                    return
                }
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
            // A Frame's title band moves it with what it holds (5.1).
            const frame = mouse.button === Qt.LeftButton ? canvas.frameTitleAt(canvas.pressWorld) : ""
            if (frame !== "") {
                canvas.pressedNode = ""
                canvas.gesture = "move"
                canvas.model.beginFrameMove(frame)
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
            case "resize":
                canvas.model.resizeBy(w.x - canvas.pressWorld.x, w.y - canvas.pressWorld.y)
                break
            case "link": {
                const port = canvas.portUnder(mouse.x, mouse.y)
                canvas.hoverProblem = port.node !== undefined
                    ? canvas.model.dragProblem(port.node, port.port, port.output, true) : ""
                canvas.updateHover(mouse.x, mouse.y)
                break
            }
            }
        }

        onReleased: mouse => {
            if (canvas.ignoredButtons & mouse.button) {
                canvas.ignoredButtons &= ~mouse.button
                return
            }
            // A right-click opens the context menu; a right-drag does nothing (12.3).
            if (mouse.button === Qt.RightButton) {
                if (canvas.gesture === "" && Math.hypot(mouse.x - canvas.rightPressAt.x, mouse.y - canvas.rightPressAt.y)
                        < Application.styleHints.startDragDistance)
                    canvas.contextMenuRequested(mouse.x, mouse.y)
                return
            }
            canvas.finishGesture(mouse.x, mouse.y, mouse.modifiers)
        }
        onCanceled: {
            canvas.ignoredButtons = 0
            canvas.finishGesture(canvas.pointer.x, canvas.pointer.y, 0)
        }

        onDoubleClicked: mouse => {
            if (mouse.button !== Qt.LeftButton)
                return
            const w = canvas.toWorld(mouse.x, mouse.y)
            // A double click on a corner returns the node to auto-size.
            const corner = canvas.cornerAt(w)
            if (corner.node !== undefined) {
                canvas.model.endResize()
                canvas.model.autoSize(corner.node)
                return
            }
            // On a group's port (its card's, or an exposed mark inside): rename it on the face (#72).
            const port = canvas.portUnder(mouse.x, mouse.y)
            if (port.node !== undefined
                    && canvas.model.portToRename(port.node, port.port, port.output).name !== undefined) {
                canvas.model.endLinkDrag()
                canvas.model.portRenameRequested(port.node, port.port, port.output)
                return
            }
            const hit = canvas.model.nodeAt(w.x, w.y)
            // On a node's title: rename it (#55).
            if (hit !== "" && port.node === undefined
                    && w.y <= canvas.model.position(hit).y + canvas.model.headerHeight) {
                canvas.model.endMove()
                canvas.model.nodeRenameRequested(hit)
                return
            }
            // A double click on a group's body enters it (5.2); its title stays for renaming.
            if (hit !== "" && canvas.model.isGroup(hit) && port.node === undefined
                    && w.y > canvas.model.position(hit).y + canvas.model.headerHeight) {
                canvas.model.endMove()
                canvas.model.enterGroup(hit)
                return
            }
            // On a Frame's title: rename it.
            const frame = hit === "" ? canvas.frameTitleAt(w) : ""
            if (frame !== "") {
                canvas.model.endMove()
                canvas.model.frameRenameRequested(frame)
                return
            }
            // A double click on a Compare Images node's picture opens the large view (#62).
            if (hit !== "" && port.node === undefined && canvas.model.isCompare(hit)
                    && canvas.model.compareResult(hit).a !== undefined) {
                canvas.model.endMove()
                canvas.openCompare(hit)
                return
            }
            // A double click on any other node's picture opens it large (#73); a Crop's opens the
            // crop window, a Compare Images', as above, its own.
            if (hit !== "" && port.node === undefined && canvas.model.pictureAt(hit, w.x, w.y)
                    && App.runs && App.runs.imageFile(hit) !== "") {
                canvas.model.endMove()
                if (canvas.model.isCrop(hit))
                    canvas.openCrop(hit)
                else
                    canvas.openImage(hit)
                return
            }
            if (hit === "" && port.node === undefined)
                canvas.searchRequested(w.x, w.y)
        }

        onWheel: wheel => {
            // A touchpad is told by its scroll phase alone. Never by the device type (Qt on
            // Wayland calls an ordinary mouse a "touchpad": seen on the owner's machine, type
            // TouchPad, phase none, angle 120, pixel 15) and never by pixelDelta (a mouse wheel
            // can carry one, and a high-resolution wheel sends fractions of a notch).
            const trackpad = wheel.phase !== Qt.NoScrollPhase
            const pixels = wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0
            const zooms = (wheel.modifiers & Qt.ControlModifier)
                || (!trackpad && canvas.navigation === "legacy")
            if (zooms) {
                const steps = (trackpad && pixels ? wheel.pixelDelta.y / 50 : wheel.angleDelta.y / 120)
                canvas.zoomAt(wheel.x, wheel.y, Math.pow(1.1, steps))
            } else if (trackpad && pixels) {
                canvas.panBy(wheel.pixelDelta.x, wheel.pixelDelta.y)
            } else {
                canvas.panBy(wheel.angleDelta.x / 2, wheel.angleDelta.y / 2)
            }
        }
    }

    // A Frame title's size in world units (#72): it zooms with the canvas, as ComfyUI's group
    // titles do, down to 10 px on screen, where it stops so it still reads.
    readonly property real frameTitleWorld: Math.max(look.frameTitleSize, Math.ceil(10 / world.scale))

    // The Frame whose title band (as tall as its title, at least a node header) holds the world
    // point, or "".
    function frameTitleAt(w) {
        const band = Math.max(model.headerHeight, frameTitleWorld + 4 * look.unit / Math.min(1, world.scale))
        return model.frameAt(w.x, w.y, band)
    }

    // The large view of a Compare Images node (#62): its viewer on the full files.
    function openCompare(node) {
        // Made on the first use: the window keeps its item budget (#5).
        if (!compareDialog)
            compareDialog = compareDialogComponent.createObject(canvas, {canvasModel: canvas.model})
        compareDialog.openFor(node)
    }
    // The large views of any node's picture and of a Crop's frame (#73), made on first use.
    function openImage(node) {
        if (!imageDialog)
            imageDialog = imageDialogComponent.createObject(canvas, {canvasModel: canvas.model})
        imageDialog.openFor(node)
    }
    // The large view of a video: the same player, bigger (#116), made on first use.
    function openMedia(node) {
        if (!mediaDialog)
            mediaDialog = mediaDialogComponent.createObject(canvas, {canvasModel: canvas.model})
        mediaDialog.openFor(node)
    }
    property var mediaDialog: null
    Component {
        id: mediaDialogComponent
        MediaDialog {}
    }
    function openCrop(node) {
        if (!cropDialog)
            cropDialog = cropDialogComponent.createObject(canvas, {canvasModel: canvas.model})
        cropDialog.openFor(node)
    }
    property var imageDialog: null
    Component {
        id: imageDialogComponent
        ImageDialog {}
    }
    property var cropDialog: null
    Component {
        id: cropDialogComponent
        CropDialog {}
    }
    function copyCompareView(node) {
        let viewer = compareDialog && compareDialog.visible && compareDialog.nodeId === node ? compareDialog.viewer : null
        for (let i = 0; !viewer && i < nodes.count; ++i) {
            const item = nodes.itemAt(i) as CanvasNode
            if (item && item.nodeId === node)
                viewer = item.compareItem
        }
        if (viewer)
            viewer.grab(result => App.imageActions.copyImage(result.image))
    }
    // Copy view (compare.copy-view): the viewer open in the large view, else the node's own.
    Connections {
        target: App.imageActions
        function onViewCopyRequested(node) { canvas.copyCompareView(node) }
    }
    property var compareDialog: null
    Component {
        id: compareDialogComponent
        CompareDialog {}
    }

    function startPan() {
        gesture = "pan"
        panStart = Qt.point(world.x, world.y)
    }

    function finishGesture(px, py, modifiers) {
        const g = gesture
        gesture = ""
        hoverPort = ({})
        switch (g) {
        case "pan":
            // A left click on empty canvas, not a drag, clears the selection.
            if (!travelled && !spaceHeld && navigation === "legacy")
                model.select([], "replace")
            rememberView()
            break
        case "resize":
            model.endResize()
            resizeCorner = ""
            break
        case "move":
            model.endMove()
            if (!travelled && pressedNode !== "") {
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
            // The model ended the drag already (an undo, an edit from a key): nothing to let go.
            if (!dragging) {
                hoverProblem = ""
                break
            }
            const from = { node: linkDrag.node, port: linkDrag.port, output: linkDrag.output }
            // Links being moved (Shift+drag) let go on nothing stay as they were: no search.
            const moving = linkDrag.targets !== undefined
            const port = portUnder(px, py)
            const w = toWorld(px, py)
            const onNode = model.nodeAt(w.x, w.y)
            if (port.node !== undefined) {
                model.endLinkDrag(port.node, port.port, port.output)
            } else if (onNode !== "" && travelled && !moving) {
                dropOnNode(onNode, px, py)
            } else {
                model.endLinkDrag()
                if (travelled && !moving && onNode === "")
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

        // Frames (5.1, 10): flat regions in a role's tint, no shadow, no header bar.
        Repeater {
            model: canvas.model.frames
            delegate: Rectangle {
                required property var modelData
                readonly property var role: canvas.look.role(modelData.color || "muted")
                x: modelData.x
                y: modelData.y
                width: modelData.width
                height: modelData.height
                radius: canvas.look.corner
                color: role.frame
                border.width: 1
                border.color: role.frameEdge
                Text {
                    // Zoomed out, the title is drawn above the nodes instead (below).
                    visible: canvas.tier === 0 && canvas.renamingFrame !== parent.modelData.id
                    x: 3 * canvas.look.unit
                    y: 2 * canvas.look.unit
                    width: parent.width - 6 * canvas.look.unit
                    text: parent.modelData.title
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    objectName: "frameTitleText"
                    color: parent.role.frameTitle
                    font.pixelSize: canvas.frameTitleWorld
                    font.weight: Font.DemiBold
                }
                // The title edited in place (a double-click on it, Rename Frame; #74).
                Loader {
                    id: frameTitleEdit
                    readonly property var frame: parent.modelData
                    readonly property color ink: parent.role.frameTitle
                    active: canvas.renamingFrame === frame.id
                    x: 3 * canvas.look.unit
                    y: 2 * canvas.look.unit
                    width: parent.width - 6 * canvas.look.unit
                    sourceComponent: TitleEditor {
                        initial: frameTitleEdit.frame.title
                        color: frameTitleEdit.ink
                        font.pixelSize: canvas.frameTitleWorld
                        font.weight: Font.DemiBold
                        onDone: keep => {
                            if (keep)
                                canvas.model.renameFrame(frameTitleEdit.frame.id, text)
                            canvas.renamingFrame = ""
                        }
                    }
                }
            }
        }

        // Comments (10): text and a rule, nothing more.
        Repeater {
            model: canvas.tier === 0 ? canvas.model.comments : []
            delegate: Item {
                required property var modelData
                x: modelData.x
                y: modelData.y
                width: Math.min(modelData.width, 58 * canvas.look.unit)
                height: commentText.height + 4
                Rectangle {
                    width: 2
                    height: parent.height
                    color: canvas.look.hairlineStrong
                }
                Text {
                    id: commentText
                    x: 2 + 2.5 * canvas.look.unit
                    y: 2
                    width: parent.width - x
                    text: parent.modelData.text
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    lineHeightMode: Text.FixedHeight
                    lineHeight: 16
                    color: canvas.look.label
                    font.pixelSize: canvas.look.textSize
                }
            }
        }

        // Wires (8): one Shape each, constant at rest; a state changes only opacity, and the
        // casing layer above draws the highlighted ones again. Dimmed while a link is dragged.
        Item {
            id: wireLayer
            visible: !canvas.model.linksHidden
            opacity: canvas.dragging ? 0.3125 : 1 // with each wire's 0.8: 0.25

            Repeater {
                id: wires
                objectName: "wires"
                model: canvas.model.links
                delegate: Shape {
                    id: wire
                    required property string path
                    required property string color
                    required property bool muted
                    opacity: muted ? 0.4 : 0.8
                    ShapePath {
                        // Width changes only with the zoom tier: 4 px in the overview.
                        strokeWidth: canvas.tier === 2 ? 4 : 2
                        strokeColor: canvas.portColor(wire.color)
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        PathSvg { path: wire.path }
                    }
                }
            }

            // The selection's wires and the hovered one: a 6 px casing in the canvas color
            // under a 2.5 px stroke. At rest this layer has no items.
            Repeater {
                model: canvas.tier === 0 ? canvas.highlightRows : []
                delegate: Shape {
                    id: lit
                    required property int modelData
                    // The wires delegate (an inline Shape with path and color), so not a type to name.
                    readonly property var wire: wires.itemAt(modelData)
                    visible: wire !== null
                    ShapePath {
                        strokeWidth: 6
                        strokeColor: canvas.look.canvas
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        PathSvg { path: lit.wire ? lit.wire.path : "" }
                    }
                    ShapePath {
                        strokeWidth: 2.5
                        strokeColor: lit.wire ? canvas.portColor(lit.wire.color) : "transparent"
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        PathSvg { path: lit.wire ? lit.wire.path : "" }
                    }
                }
            }

            // Into a running node (8.2): a 7 px dot in the wire's color with a 2 px canvas ring
            // travels the wire every 900 ms. Only while a run is going, at the full tier.
            Repeater {
                objectName: "runningDots"
                model: canvas.windowRuns && canvas.tier === 0 ? canvas.windowRuns.runningWires : []
                delegate: Item {
                    id: dot
                    required property var modelData
                    PathInterpolator {
                        id: along
                        path: Path {
                            PathSvg { path: dot.modelData.path }
                        }
                        NumberAnimation on progress {
                            from: 0
                            to: 1
                            duration: 900
                            loops: Animation.Infinite
                        }
                    }
                    Rectangle {
                        x: along.x - 5.5
                        y: along.y - 5.5
                        width: 11
                        height: 11
                        radius: 5.5
                        color: canvas.look.canvas
                        Rectangle {
                            x: 2
                            y: 2
                            width: 7
                            height: 7
                            radius: 3.5
                            color: canvas.portColor(dot.modelData.color)
                        }
                    }
                }
            }

            // Reroute points: a 10 px dot in the wire's color with a 2 px canvas ring.
            Repeater {
                model: canvas.tier <= 1 ? canvas.model.reroutes : []
                delegate: Rectangle {
                    required property var modelData
                    x: modelData.x - 7
                    y: modelData.y - 7
                    width: 14
                    height: 14
                    radius: 7
                    color: canvas.look.canvas
                    Rectangle {
                        x: 2
                        y: 2
                        width: 10
                        height: 10
                        radius: 5
                        color: canvas.portColor(parent.modelData.color)
                    }
                }
            }
        }

        // Node shadows (7): one pre-painted 9-slice texture per level, shared by every node,
        // in their own layer so they draw in one batch; off below the readable zoom. A node at
        // rest is level 1, a selected one 2, and the selected nodes being dragged 4 (lifted).
        Item {
            id: shadowLayer
            objectName: "shadows"
            visible: canvas.tier === 0
            readonly property bool lifting: canvas.gesture === "move" && canvas.travelled
            Repeater {
                model: canvas.model.nodes
                delegate: BorderImage {
                    required property real posX
                    required property real posY
                    required property real nodeWidth
                    required property real nodeHeight
                    required property bool selected
                    required property string mode
                    readonly property int level: selected ? (shadowLayer.lifting ? 4 : 2) : 1
                    readonly property int reach: canvas.shadowMargins[level - 1]
                    readonly property int edge: reach + canvas.look.corner + 1
                    x: posX - reach
                    y: posY - reach
                    width: nodeWidth + 2 * reach
                    height: nodeHeight + 2 * reach
                    opacity: mode === "mute" ? 0.4 : 1
                    source: level === 4 ? canvas.look.shadow4 : level === 2 ? canvas.look.shadow2 : canvas.look.shadow1
                    border.left: edge
                    border.top: edge
                    border.right: edge
                    border.bottom: edge
                    smooth: true
                }
            }
        }

        Repeater {
            id: nodes
            objectName: "nodes"
            model: canvas.model.nodes
            delegate: CanvasNode {
                canvasModel: canvas.model
                canvasView: canvas
                look: canvas.look
                linkDrag: canvas.linkDrag
                detailArea: canvas.detailArea
                tier: canvas.tier
                hovered: canvas.hoveredNode === nodeId
                hoverKey: canvas.hoverPort.node === nodeId ? (canvas.hoverPort.output ? "o:" : "i:") + canvas.hoverPort.port : ""
                activeCorner: canvas.gesture === "resize" ? (canvas.pressedNode === nodeId ? canvas.resizeCorner : "")
                            : canvas.hoverCorner.node === nodeId ? canvas.hoverCorner.corner : ""
            }
        }

        // Zoomed out, frame titles sit above the nodes on their frame's fill, so the frames
        // read as the map; at least 10 px on screen.
        Repeater {
            model: canvas.tier > 0 ? canvas.model.frames : []
            delegate: Rectangle {
                required property var modelData
                readonly property var role: canvas.look.role(modelData.color || "muted")
                visible: canvas.renamingFrame !== modelData.id // edited in place below (#74)
                readonly property real s: 1 / world.scale
                x: modelData.x + 3 * canvas.look.unit
                y: modelData.y + 2 * canvas.look.unit
                width: Math.min(titleText.implicitWidth + 4 * s, modelData.width - 6 * canvas.look.unit)
                height: titleText.implicitHeight
                color: role.frame
                Text {
                    id: titleText
                    x: 2 * parent.s
                    width: parent.width - 4 * parent.s
                    text: parent.modelData.title
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    objectName: "frameTitleText"
                    color: parent.role.frameTitle
                    font.pixelSize: canvas.frameTitleWorld
                    font.weight: Font.DemiBold
                }
            }
        }

        // The valid target under a dragged link: a 2 px accent ring outside a 2 px body gap
        // around the port's grown mark (docs/canvas-look.md 5), 14 px filled or 10 hollow.
        Rectangle {
            id: targetRing
            readonly property string shape: canvas.hoverPort.shape || "circle"
            readonly property bool round: shape === "circle" || shape === "ring"
            readonly property int mark: canvas.hoverPort.wired && shape !== "ring" ? 14 : 10
            visible: canvas.dragging && canvas.hoverPort.node !== undefined && canvas.hoverProblem === ""
            x: (canvas.hoverPort.x || 0) - width / 2
            y: (canvas.hoverPort.y || 0) - height / 2
            width: Math.round(mark * 1.4) + 8
            height: width
            radius: round ? width / 2 : Math.min(canvas.look.corner, 2) + 4
            rotation: shape === "diamond" ? 45 : 0
            scale: shape === "diamond" ? 0.8 : 1
            color: "transparent"
            border.width: 2
            border.color: canvas.look.accent
            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                radius: targetRing.round ? width / 2 : Math.max(0, targetRing.radius - 2)
                color: "transparent"
                border.width: 2
                border.color: canvas.look.body
            }
        }

        // The link being dragged, over the nodes: from its port to the pointer, or snapped to
        // the port under it when that can take it; dashed in the error color when it cannot.
        Shape {
            id: draggedLink
            objectName: "draggedLink"
            // The default (geometry) renderer drew this wire as only its casing after a frame
            // had been drawn with the pointer still on the start port (a degenerate curve).
            preferredRendererType: Shape.CurveRenderer
            visible: canvas.dragging
            readonly property bool invalid: canvas.hoverProblem !== ""
            readonly property bool snapped: canvas.hoverPort.node !== undefined && !invalid
            readonly property point end: snapped ? Qt.point(canvas.hoverPort.x, canvas.hoverPort.y)
                                                 : canvas.toWorld(canvas.pointer.x, canvas.pointer.y)
            readonly property point start: Qt.point(canvas.linkDrag.x || 0, canvas.linkDrag.y || 0)
            // Always drawn from the output side to the input side.
            readonly property point a: canvas.linkDrag.output ? start : end
            readonly property point b: canvas.linkDrag.output ? end : start
            readonly property real dx: canvas.bend(a.x, b.x)
            ShapePath {
                strokeWidth: draggedLink.invalid ? 0 : 6
                strokeColor: draggedLink.invalid ? "transparent" : canvas.look.canvas
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                startX: draggedLink.a.x
                startY: draggedLink.a.y
                PathCubic {
                    x: draggedLink.b.x
                    y: draggedLink.b.y
                    control1X: draggedLink.a.x + draggedLink.dx
                    control1Y: draggedLink.a.y
                    control2X: draggedLink.b.x - draggedLink.dx
                    control2Y: draggedLink.b.y
                }
            }
            ShapePath {
                strokeWidth: 2.5
                strokeColor: draggedLink.invalid ? canvas.look.error : canvas.portColor(canvas.linkDrag.color || "any")
                strokeStyle: draggedLink.invalid ? ShapePath.DashLine : ShapePath.SolidLine
                dashPattern: [2.4, 2]
                fillColor: "transparent"
                capStyle: ShapePath.FlatCap
                startX: draggedLink.a.x
                startY: draggedLink.a.y
                PathCubic {
                    x: draggedLink.b.x
                    y: draggedLink.b.y
                    control1X: draggedLink.a.x + draggedLink.dx
                    control1Y: draggedLink.a.y
                    control2X: draggedLink.b.x - draggedLink.dx
                    control2Y: draggedLink.b.y
                }
            }
        }
    }

    // The hovered wire's cut handle at its midpoint: a click removes the link.
    Rectangle {
        id: cutHandle
        objectName: "cutHandle"
        readonly property real cx: world.x + (canvas.hoverLink.midX || 0) * world.scale
        readonly property real cy: world.y + (canvas.hoverLink.midY || 0) * world.scale
        visible: canvas.hoverLink.row !== undefined && canvas.gesture === "" && canvas.tier === 0
        x: cx - width / 2
        y: cy - height / 2
        width: 18
        height: 18
        radius: 9
        color: canvas.look.popup
        border.width: 1
        border.color: canvas.look.hairlineStrong
        Glyph {
            anchors.centerIn: parent
            name: "x"
            look: canvas.look
            color: canvas.look.text
            font.pixelSize: 12
        }
    }

    // The marquee, in canvas coordinates.
    Rectangle {
        visible: canvas.gesture === "marquee" && canvas.travelled
        x: Math.min(canvas.pressAt.x, canvas.pointer.x)
        y: Math.min(canvas.pressAt.y, canvas.pointer.y)
        width: Math.abs(canvas.pointer.x - canvas.pressAt.x)
        height: Math.abs(canvas.pointer.y - canvas.pressAt.y)
        color: Qt.alpha(canvas.look.accent, 0.1)
        border.width: 1
        border.color: canvas.look.accent
    }

    // Why the port under a dragged link cannot take it (12.6, docs/canvas-look.md 8.2).
    Item {
        id: reason
        objectName: "linkProblem"
        visible: canvas.gesture === "link" && canvas.hoverProblem !== ""
        x: Math.min(canvas.pointer.x + 14, canvas.width - width - 8)
        y: Math.min(canvas.pointer.y + 14, canvas.height - height - 8)
        width: Math.min(problemText.implicitWidth + 14 + 3.5 * canvas.look.unit + 2, 380)
        height: Math.max(problemText.implicitHeight, 14) + 2.5 * canvas.look.unit + 2
        Shadow {
            target: reasonBox
            level: 3
            look: canvas.look
        }
        Rectangle {
            id: reasonBox
            anchors.fill: parent
            radius: Math.min(canvas.look.popupCorner, 6)
            color: canvas.look.popup
            border.width: 1
            border.color: canvas.look.error
            Glyph {
                x: 1.75 * canvas.look.unit
                anchors.verticalCenter: parent.verticalCenter
                name: "circle-x"
                look: canvas.look
                color: canvas.look.error
            }
            Text {
                id: problemText
                x: 1.75 * canvas.look.unit + 14 + 1.5 * canvas.look.unit
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, 380 - x - 2.25 * canvas.look.unit)
                text: canvas.hoverProblem // the type names bold
                textFormat: Text.StyledText
                wrapMode: Text.Wrap
                color: canvas.look.text
                font.pixelSize: canvas.look.labelSize
            }
        }
    }

    // A wire let go on a node's body (#74, as ComfyUI): it connects to the one port there that
    // fits (the one free one, when an input has a link already), and when several free ones do a
    // short list asks which; the drag stays open until a pick, Esc or a click elsewhere.
    property var dropChoice: ({})
    Item {
        id: lens
        objectName: "lens"
        readonly property real pad: 2 * canvas.look.unit
        // Read again when a node changes (a resize).
        readonly property rect box: canvas.lensOpen && canvas.nodesTick >= 0 ? canvas.model.nodeRect(canvas.lensNode)
                                                                               : Qt.rect(0, 0, 0, 0)
        // Centred on the node where it sits on screen, inside the canvas.
        readonly property real cx: world.x + (box.x + box.width / 2) * world.scale
        readonly property real cy: world.y + (box.y + box.height / 2) * world.scale
        visible: canvas.lensOpen
        z: 9
        width: box.width * lensWorld.scale + 2 * pad
        height: box.height * lensWorld.scale + 2 * pad
        x: Math.max(8, Math.min(cx - width / 2, canvas.width - width - 8))
        y: Math.max(8, Math.min(cy - height / 2, canvas.height - height - 8))
        Shadow {
            target: lensBox
            level: 3
            look: canvas.look
        }
        Rectangle {
            id: lensBox
            anchors.fill: parent
            radius: Math.min(canvas.look.popupCorner, 12)
            color: canvas.look.canvas
            border.width: 1
            border.color: canvas.look.hairlineStrong
        }
        Item {
            anchors.fill: parent
            clip: true
            Item {
                id: lensWorld
                objectName: "lensWorld"
                transformOrigin: Item.TopLeft
                scale: 1 // readable: the node at zoom 1
                x: lens.pad - lens.box.x * scale
                y: lens.pad - lens.box.y * scale
                Repeater {
                    model: canvas.lensOpen ? canvas.model.lensNodes : null
                    delegate: CanvasNode {
                        canvasModel: canvas.model
                        canvasView: canvas
                        look: canvas.look
                        linkDrag: canvas.linkDrag
                        detailArea: Qt.rect(-1e7, -1e7, 2e7, 2e7)
                        tier: 0
                        hovered: canvas.hoveredNode === nodeId
                        hoverKey: canvas.hoverPort.node === nodeId
                                  ? (canvas.hoverPort.output ? "o:" : "i:") + canvas.hoverPort.port : ""
                        activeCorner: ""
                    }
                }
            }
        }
    }
    function dropOnNode(node, px, py) {
        const targets = model.dropTargets(node)
        const free = targets.filter(t => t.free)
        if (targets.length === 0) {
            model.endLinkDrag()
            return
        }
        if (targets.length === 1 || free.length === 1) {
            const t = free.length === 1 ? free[0] : targets[0]
            model.endLinkDrag(node, t.port, t.output)
            return
        }
        dropChoice = {node: node, targets: targets, x: px, y: py}
    }
    // The drag ended another way (an edit, an undo): the list goes with it.
    Connections {
        target: canvas.model
        function onLinkDragChanged() {
            if (canvas.dropChoice.node !== undefined && canvas.model.linkDrag.node === undefined)
                canvas.dropChoice = ({})
        }
    }
    function chooseDrop(target) {
        const node = dropChoice.node
        dropChoice = ({})
        if (target)
            model.endLinkDrag(node, target.port, target.output)
        else
            model.endLinkDrag()
    }
    Item {
        id: dropMenu
        objectName: "dropChooser"
        visible: canvas.dropChoice.node !== undefined
        z: 10
        x: Math.max(8, Math.min((canvas.dropChoice.x || 0) + 8, canvas.width - width - 8))
        y: Math.max(8, Math.min((canvas.dropChoice.y || 0) + 8, canvas.height - height - 8))
        width: 52 * canvas.look.unit
        height: dropColumn.implicitHeight + 2 * canvas.look.unit
        Shadow {
            target: dropBox
            level: 3
            look: canvas.look
        }
        Rectangle {
            id: dropBox
            anchors.fill: parent
            radius: Math.min(canvas.look.popupCorner, 8)
            color: canvas.look.popup
            border.width: 1
            border.color: canvas.look.hairlineStrong
            Column {
                id: dropColumn
                x: canvas.look.unit
                y: canvas.look.unit
                width: parent.width - 2 * canvas.look.unit
                Repeater {
                    model: canvas.dropChoice.targets || []
                    delegate: Rectangle {
                        id: dropRow
                        required property var modelData
                        objectName: "dropTarget-" + modelData.port
                        width: dropColumn.width
                        height: 7 * canvas.look.unit
                        radius: canvas.look.smallCorner
                        color: dropArea.containsMouse ? canvas.look.menuRow : "transparent"
                        Text {
                            x: 2 * canvas.look.unit
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 4 * canvas.look.unit
                            elide: Text.ElideRight
                            text: dropRow.modelData.free ? dropRow.modelData.label
                                                         : qsTr("%1 (replaces its link)").arg(dropRow.modelData.label)
                            color: canvas.look.text
                            font.pixelSize: canvas.look.labelSize
                        }
                        MouseArea {
                            id: dropArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: canvas.chooseDrop(dropRow.modelData)
                        }
                    }
                }
            }
        }
    }

    // The selection toolbar (#74, as ComfyUI's selection toolbox): over the selection, its common
    // commands, Enter on a group and Open Image on a node with one, and "…" for the node's menu.
    // Each button is a command, shown and enabled as the menus say. Made only while shown.
    property int nodesTick: 0 // the nodes' changes (a move, a bypass, a new image): read again
    Connections {
        target: canvas.model.nodes
        function onDataChanged() { canvas.nodesTick++ }
        function onRowsInserted() { canvas.nodesTick++ }
        function onRowsRemoved() { canvas.nodesTick++ }
        function onModelReset() { canvas.nodesTick++ }
    }
    // A new selection changes which commands apply once the registry has heard of it too.
    Connections {
        target: canvas.model
        function onSelectionChanged() { Qt.callLater(() => canvas.nodesTick++) }
    }
    Loader {
        active: canvas.selectionToolbarShown && canvas.model.selection.length > 0 && canvas.gesture === "" && canvas.renamingNode === ""
                && canvas.renamingFrame === ""
                && canvas.dropChoice.node === undefined && !canvas.lensOpen
        z: 8
        sourceComponent: Item {
            id: toolbar
            objectName: "selectionToolbar"
            readonly property var selection: canvas.model.selection
            readonly property var target: selection.length === 1 ? {node: selection[0]} : ({})
            // The selection's box on screen, read again when a node changes (nodesTick).
            readonly property rect box: canvas.nodesTick >= 0 ? boxOf(selection) : Qt.rect(0, 0, 0, 0)
            function boxOf(ids) {
                let left = Infinity, top = Infinity, right = -Infinity, bottom = -Infinity
                for (const id of ids) {
                    const r = canvas.model.nodeRect(id)
                    if (r.width <= 0)
                        continue
                    left = Math.min(left, r.x)
                    top = Math.min(top, r.y)
                    right = Math.max(right, r.x + r.width)
                    bottom = Math.max(bottom, r.y + r.height)
                }
                if (left === Infinity)
                    return Qt.rect(0, 0, 0, 0)
                return Qt.rect(world.x + left * world.scale, world.y + top * world.scale,
                               (right - left) * world.scale, (bottom - top) * world.scale)
            }
            visible: box.width > 0
            width: buttons.implicitWidth + 2 * canvas.look.unit
            height: 9 * canvas.look.unit
            x: Math.max(8, Math.min(box.x + box.width / 2 - width / 2, canvas.width - width - 8))
            // Above the selection; below it when there is no room above.
            y: box.y - height - 2 * canvas.look.unit >= 8 ? box.y - height - 2 * canvas.look.unit
                                                           : Math.min(box.y + box.height + 2 * canvas.look.unit,
                                                                      canvas.height - height - 8)
            // No shadow: it would fall on the selection just under it, its ring included.
            Rectangle {
                id: toolbarBox
                anchors.fill: parent
                radius: Math.min(canvas.look.popupCorner, 10)
                color: canvas.look.popup
                border.width: 1
                border.color: canvas.look.hairline
            }
            Row {
                id: buttons
                anchors.centerIn: parent
                spacing: canvas.look.unit / 2
                Repeater {
                    // Always there: Bypass, Frame, Delete (off when they cannot act), the colors and
                    // the menu; the rest only where they apply.
                    model: [{command: "run.to-here", glyph: "play"},
                            {command: "node.bypass", glyph: "corner-down-right"},
                            {command: "colors", glyph: "palette"}, {command: "frame.make", glyph: "frame"},
                            {command: "group.enter", glyph: "log-in"}, {command: "node.open-image", glyph: "image"},
                            {command: "edit.delete", glyph: "trash-2"}, {command: "more", glyph: "ellipsis"}]
                    delegate: Rectangle {
                        id: button
                        required property var modelData
                        readonly property bool menu: modelData.command === "colors" || modelData.command === "more"
                        // Read again when a node or the selection changes (nodesTick).
                        readonly property var entry: canvas.nodesTick < 0 ? ({})
                            : menu ? ({available: true, enabled: true, label: ""})
                            : App.commands.entry(modelData.command, toolbar.target)
                        readonly property bool always: menu || ["node.bypass", "frame.make", "edit.delete"]
                                                                 .indexOf(modelData.command) >= 0
                        objectName: "toolbar-" + modelData.command
                        visible: entry.available === true && (always || entry.enabled === true)
                        width: 7 * canvas.look.unit
                        height: 7 * canvas.look.unit
                        radius: canvas.look.smallCorner
                        color: buttonArea.containsMouse && entry.enabled ? canvas.look.menuRow : "transparent"
                        opacity: entry.enabled ? 1 : 0.4
                        Accessible.role: Accessible.Button
                        Accessible.name: entry.label || modelData.command
                        Glyph {
                            anchors.centerIn: parent
                            look: canvas.look
                            name: button.modelData.glyph
                            color: buttonArea.containsMouse && button.entry.enabled ? canvas.look.text
                                                                                    : canvas.look.label
                        }
                        MouseArea {
                            id: buttonArea
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: button.entry.enabled === true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                const at = button.mapToItem(canvas, 0, button.height + canvas.look.unit)
                                if (button.modelData.command === "more")
                                    canvas.menuRequested("node", at.x, at.y)
                                else if (button.modelData.command === "colors")
                                    canvas.menuRequested("colors", at.x, at.y)
                                else
                                    App.commands.triggerOn(button.modelData.command, toolbar.target)
                            }
                        }
                    }
                }
            }
        }
    }

    // The node or Frame whose title is edited in place (#74), or "".
    property string renamingNode: ""
    property string renamingFrame: ""
    Connections {
        target: canvas.model
        function onNodeRenameRequested(node) { canvas.renamingNode = node }
        function onFrameRenameRequested(frame) { canvas.renamingFrame = frame }
    }
    // A node's warnings (docs/canvas-look.md 9, #72): one card for the canvas, shown while a
    // node's warning count is hovered, kept up by a click until a click elsewhere, Esc or a change
    // of level. Its text can be selected and copied.
    property string warningsNode: ""
    property bool warningsPinned: false
    function showWarnings(nodeId, warnings, badge, pin) {
        if (warningsPinned && !pin)
            return // a card kept up by a click stays as it is while other badges are hovered
        warningsPinned = pin
        warningsNode = nodeId
        warningsTip.text = warnings.join("\n")
        const at = badge.mapToItem(canvas, badge.width, badge.height)
        warningsTip.x = Math.max(8, Math.min(at.x - warningsTip.width, canvas.width - warningsTip.width - 8))
        warningsTip.y = Math.min(at.y + canvas.look.unit, canvas.height - warningsTip.height - 8)
    }
    function hideWarnings() {
        if (!warningsPinned)
            warningsNode = ""
    }
    function closeWarnings() {
        warningsPinned = false
        warningsNode = ""
    }
    Connections {
        target: canvas.model
        function onScopeChanged() { canvas.closeWarnings() }
    }

    Item {
        id: warningsTip
        objectName: "warningsTip"
        property alias text: warningsText.text
        visible: canvas.warningsNode !== ""
        z: 10
        width: Math.min(warningsText.implicitWidth, 360) + 3.5 * canvas.look.unit + 14 + 2
        height: warningsText.height + 2.5 * canvas.look.unit + 2
        Shadow {
            target: warningsBox
            level: 3
            look: canvas.look
        }
        Rectangle {
            id: warningsBox
            anchors.fill: parent
            radius: Math.min(canvas.look.popupCorner, 6)
            color: canvas.look.popup
            border.width: 1
            border.color: canvas.look.hairlineStrong
            Glyph {
                x: 1.75 * canvas.look.unit
                y: 1.25 * canvas.look.unit + 1
                name: "triangle-alert"
                look: canvas.look
                color: canvas.look.label
            }
            TextEdit {
                id: warningsText
                x: 1.75 * canvas.look.unit + 14 + 1.5 * canvas.look.unit
                y: 1.25 * canvas.look.unit + 1
                width: Math.min(implicitWidth, 360)
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                textFormat: TextEdit.PlainText
                color: canvas.look.text
                selectionColor: canvas.look.accent
                selectedTextColor: canvas.look.accentForeground
                font.pixelSize: canvas.look.labelSize
            }
        }
    }

    // A node's error chip was clicked (4.7): the run pill lists the errors, and the node comes
    // to the middle of the view, selected, as a click on its row there does.
    function showNodeError(nodeId) {
        if (!canvas.windowRuns)
            return
        const pill = runPill.item as RunPill
        if (pill)
            pill.listOpen = true
        canvas.windowRuns.showError(nodeId)
    }

    // Inside a group (5.2, docs/canvas-look.md 10): where the canvas is, outside in, a pill as the
    // zoom and run ones are; a click on a level goes back to it.
    Item {
        objectName: "breadcrumb"
        visible: canvas.breadcrumbShown && canvas.model.inGroup
        x: 4 * canvas.look.unit
        y: 4 * canvas.look.unit
        width: crumbBox.width
        height: crumbBox.height
        Shadow {
            target: crumbBox
            level: 2
            look: canvas.look
        }
        Rectangle {
            id: crumbBox
            width: crumbs.implicitWidth + 6 * canvas.look.unit
            height: 7 * canvas.look.unit
            radius: Math.min(canvas.look.popupCorner, 14)
            color: canvas.look.popup
            border.width: 1
            border.color: canvas.look.hairline
        }
        Row {
            id: crumbs
            x: 3 * canvas.look.unit
            anchors.verticalCenter: crumbBox.verticalCenter
            spacing: canvas.look.unit
            Repeater {
                model: [{id: "", title: qsTr("Workflow")}].concat(canvas.model.breadcrumb)
                delegate: Row {
                    id: crumb
                    required property var modelData
                    required property int index
                    readonly property bool last: index === canvas.model.breadcrumb.length
                    spacing: canvas.look.unit
                    Text {
                        visible: crumb.index > 0
                        anchors.verticalCenter: parent.verticalCenter
                        text: "\u203a"
                        color: canvas.look.label
                        font.pixelSize: canvas.look.labelSize
                    }
                    Glyph {
                        visible: crumb.index > 0
                        anchors.verticalCenter: parent.verticalCenter
                        look: canvas.look
                        name: "layers"
                        color: crumb.last ? canvas.look.text : canvas.look.label
                        font.pixelSize: 12
                    }
                    Text {
                        objectName: "crumb" + crumb.index
                        anchors.verticalCenter: parent.verticalCenter
                        text: crumb.modelData.title
                        textFormat: Text.PlainText
                        color: crumb.last ? canvas.look.text : canvas.look.label
                        font.pixelSize: canvas.look.labelSize
                        font.weight: crumb.last ? Font.DemiBold : Font.Normal
                        font.underline: !crumb.last && crumbArea.containsMouse
                        MouseArea {
                            id: crumbArea
                            anchors.fill: parent
                            enabled: !crumb.last
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: canvas.model.leaveTo(crumb.index)
                        }
                    }
                }
            }
        }
    }

    // The run, top right (11).
    Loader {
        id: runPill
        objectName: "runPill"
        active: canvas.runPillShown && canvas.windowRuns !== null
        x: canvas.width - width - 4 * canvas.look.unit
        y: 4 * canvas.look.unit
        sourceComponent: RunPill {
            look: canvas.look
            runs: canvas.windowRuns
        }
    }

    // The zoom, bottom left (11).
    Item {
        objectName: "zoomPill"
        visible: canvas.zoomPillShown
        x: 4 * canvas.look.unit
        y: canvas.height - height - 4 * canvas.look.unit
        width: zoomText.implicitWidth + 6 * canvas.look.unit
        height: 7 * canvas.look.unit
        Shadow {
            target: zoomBox
            level: 2
            look: canvas.look
        }
        Rectangle {
            id: zoomBox
            anchors.fill: parent
            radius: Math.min(canvas.look.popupCorner, 14)
            color: canvas.look.popup
            border.width: 1
            border.color: canvas.look.hairline
            Text {
                id: zoomText
                anchors.centerIn: parent
                text: Math.round(world.scale * 100) + "%"
                color: canvas.look.label
                font.pixelSize: canvas.look.textSize
                font.features: { "tnum": 1 }
            }
        }
    }
}
