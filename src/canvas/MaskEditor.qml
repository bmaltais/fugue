pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import Fugue

// The mask editor (#172, spec 14.4): a picture at full size to paint a mask on. A LargeViewDialog
// whose viewer is CompareView in `single` mode with a MaskLayer over it. It knows nothing of nodes:
// give it the picture and, if there is one, the mask to start from; it says what was saved.
//
// Input:  imageSource (the picture), imageSize (its size in pixels, which the mask is in), maskPath
//         (the stored mask, an 8-bit gray PNG, or empty), look, and `stem` (the name the saved file
//         starts with; by default the picture's, then "-mask"). Then open().
// Output: saved(path) on OK: the mask stored in the inputs folder (`folder`, when a caller wants
//         another), or an empty path when nothing is painted (no mask); canceled() on Cancel, the
//         close button or Escape. Each open starts from `maskPath`, so a cancel drops the session.
//
// Tools: Brush (B), Eraser (E, X swaps them), Lasso (L), Rectangle (R); Alt with a shape erases.
// Invert (I), Fill, Clear (Shift+Delete). Size [ and ], hardness Shift+[ and Shift+], or Alt+drag
// with the brush (left and right sizes, up and down sets hardness). Ctrl+Z undoes, Ctrl+Shift+Z or
// Ctrl+Y redoes. H shows the mask alone. The wheel zooms, a middle drag or Space+drag pans, 0 fits
// and 1 is 100% (the dialog's). Enter is OK, Escape is Cancel.
LargeViewDialog {
    id: root
    objectName: "maskEditor"
    name: "mask"

    property url imageSource
    property size imageSize: Qt.size(0, 0)
    property string maskPath
    property string stem: {
        const file = String(imageSource).split("?")[0].split("/").pop()
        const base = decodeURIComponent(file).replace(/\.[^.]*$/, "")
        return (base === "" ? "image" : base) + "-mask"
    }
    property alias folder: paintLayer.folder
    signal saved(string path)
    signal canceled

    // What the pointer does, and the brush: its size in picture pixels, its hardness (0 to 1).
    property string tool: "brush" // brush, eraser, lasso, rect
    property real brushSize: 64
    property real brushHardness: 0.8
    property bool maskOnly: false
    readonly property real maxBrush: Math.max(64, Math.min(2048, Math.max(imageSize.width, imageSize.height)))
    readonly property alias layer: paintLayer
    readonly property alias viewer: pictureViewer
    readonly property bool erasing: tool === "eraser"
    property string error
    property bool spaceHeld: false
    property bool done: false

    zoomable: pictureViewer

    function setTool(name) { tool = name }
    function swapTools() { tool = tool === "eraser" ? "brush" : "eraser" }
    function setSize(v) { brushSize = Math.max(1, Math.min(maxBrush, Math.round(v))) }
    function setHardness(v) { brushHardness = Math.max(0, Math.min(1, Math.round(v * 100) / 100)) }
    function stepSize(up) { setSize(up ? Math.max(brushSize + 1, brushSize * 1.25) : Math.min(brushSize - 1, brushSize / 1.25)) }
    function stepHardness(up) { setHardness(brushHardness + (up ? 0.1 : -0.1)) }
    function toggleMaskOnly() { maskOnly = !maskOnly }
    // OK: the mask is stored and its path said; nothing painted is no file, an empty path.
    function accept() {
        error = ""
        let path = ""
        if (!paintLayer.empty) {
            path = paintLayer.store(stem)
            if (path === "") {
                error = paintLayer.lastError() !== "" ? paintLayer.lastError() : qsTr("The mask could not be saved.")
                return
            }
        }
        done = true
        saved(path)
        close()
    }
    function cancel() { close() }

    onAboutToShow: {
        done = false
        error = ""
        maskOnly = false
        spaceHeld = false
        paintLayer.imageSize = imageSize
        paintLayer.reset()
        if (maskPath !== "" && !paintLayer.loadMask(maskPath))
            error = paintLayer.lastError()
        pictureViewer.fit()
    }
    onClosed: if (!done) canceled()
    onConfirm: accept()

    // The editor's keys, after the dialog's own (0, 1, + and -, Enter) did not take the key.
    onKeyPressed: event => {
        const ctrl = event.modifiers & Qt.ControlModifier
        const shift = event.modifiers & Qt.ShiftModifier
        if (event.key === Qt.Key_Space) {
            if (!event.isAutoRepeat) spaceHeld = true
        } else if (ctrl) {
            if (event.key === Qt.Key_Z && !shift) paintLayer.undo()
            else if ((event.key === Qt.Key_Z && shift) || event.key === Qt.Key_Y) paintLayer.redo()
            else return
        } else if (event.modifiers & (Qt.AltModifier | Qt.MetaModifier)) {
            return
        } else if (event.key === Qt.Key_B) tool = "brush"
        else if (event.key === Qt.Key_E) tool = "eraser"
        else if (event.key === Qt.Key_X) swapTools()
        else if (event.key === Qt.Key_L) tool = "lasso"
        else if (event.key === Qt.Key_R) tool = "rect"
        else if (event.key === Qt.Key_I) paintLayer.invert()
        else if (event.key === Qt.Key_H) toggleMaskOnly()
        else if (event.key === Qt.Key_Delete && shift) paintLayer.clear()
        else if (event.key === Qt.Key_BracketLeft || event.key === Qt.Key_BraceLeft) { if (shift) stepHardness(false); else stepSize(false) }
        else if (event.key === Qt.Key_BracketRight || event.key === Qt.Key_BraceRight) { if (shift) stepHardness(true); else stepSize(true) }
        else return
        event.accepted = true
    }
    onKeyReleased: event => {
        if (event.key === Qt.Key_Space && !event.isAutoRepeat)
            spaceHeld = false
    }

    // ---- the bar: tools | brush | edits | view ------------------------------------------------

    tools: Row {
        spacing: root.look.unit
        // A one-key tool of the bar.
        component ToolButton: ViewButton {
            required property string toolName
            look: root.look
            current: root.tool === toolName
            onTapped: root.tool = toolName
        }
        ToolButton { objectName: "maskTool-brush"; toolName: "brush"; glyph: "brush"; tip: qsTr("Brush (B)") }
        ToolButton { objectName: "maskTool-eraser"; toolName: "eraser"; glyph: "eraser"; tip: qsTr("Eraser (E, X swaps)") }
        ToolButton { objectName: "maskTool-lasso"; toolName: "lasso"; glyph: "lasso-select"; tip: qsTr("Lasso (L, Alt erases)") }
        ToolButton { objectName: "maskTool-rect"; toolName: "rect"; glyph: "square-dashed"; tip: qsTr("Rectangle (R, Alt erases)") }
        Item { width: root.look.unit; height: 1 }
        ValueSlider {
            objectName: "maskSize"
            label: qsTr("Size")
            from: 1
            to: root.maxBrush
            value: root.brushSize
            shown: qsTr("%1 px").arg(Math.round(root.brushSize))
            curved: true
            step: 1
            tip: qsTr("Brush size in picture pixels ([ and ])")
            onMoved: v => root.setSize(v)
        }
        ValueSlider {
            objectName: "maskHardness"
            label: qsTr("Hardness")
            from: 0
            to: 1
            value: root.brushHardness
            shown: qsTr("%1%").arg(Math.round(root.brushHardness * 100))
            step: 0.01
            tip: qsTr("Brush hardness (Shift+[ and Shift+])")
            onMoved: v => root.setHardness(v)
        }
        Item { width: root.look.unit; height: 1 }
        ViewButton {
            objectName: "maskUndo"
            look: root.look
            glyph: "undo-2"
            tip: qsTr("Undo (Ctrl+Z)")
            enabled: paintLayer.canUndo
            onTapped: paintLayer.undo()
        }
        ViewButton {
            objectName: "maskRedo"
            look: root.look
            glyph: "redo-2"
            tip: qsTr("Redo (Ctrl+Shift+Z)")
            enabled: paintLayer.canRedo
            onTapped: paintLayer.redo()
        }
        Item { width: root.look.unit; height: 1 }
        ViewButton {
            objectName: "maskInvert"
            look: root.look
            glyph: "contrast"
            tip: qsTr("Invert (I)")
            onTapped: paintLayer.invert()
        }
        ViewButton {
            objectName: "maskFill"
            look: root.look
            glyph: "paint-bucket"
            tip: qsTr("Fill the whole picture")
            onTapped: paintLayer.fill()
        }
        ViewButton {
            objectName: "maskOnly"
            look: root.look
            glyph: "eye"
            tip: qsTr("Show the mask alone (H)")
            current: root.maskOnly
            onTapped: root.toggleMaskOnly()
        }
        // The destructive one, apart from the rest of the tools by twice the gap, and undoable.
        Item { width: root.look.unit; height: 1 }
        ViewButton {
            objectName: "maskClear"
            look: root.look
            glyph: "trash-2"
            tip: qsTr("Clear the mask (Shift+Delete)")
            danger: true
            onTapped: paintLayer.clear()
        }
    }

    // A slider in the bar: a well with its fill from the left, the label and the value over it
    // (the node's slider widget, canvas-look 4.4). A press or drag sets it, Left and Right step it
    // (Shift: ten steps), the wheel too.
    component ValueSlider: Rectangle {
        id: slider
        property string label
        property string shown
        property string tip
        property real from: 0
        property real to: 1
        property real value: 0
        property real step: 1
        property bool curved: false // a square-law scale, finer at the small end
        signal moved(real v)
        readonly property real fraction: to > from ? Math.max(0, Math.min(1, (value - from) / (to - from))) : 0
        readonly property real position: curved ? Math.sqrt(fraction) : fraction
        function valueAt(x) {
            const f = Math.max(0, Math.min(1, x / width))
            return from + (to - from) * (curved ? f * f : f)
        }
        width: 26 * root.look.unit
        height: 7 * root.look.unit
        radius: root.look.smallCorner
        color: root.look.well
        border.width: activeFocus ? 2 : 0
        border.color: root.look.accent
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: label
        Accessible.description: shown
        Rectangle {
            width: slider.width * slider.position
            height: slider.height
            radius: root.look.smallCorner
            color: Qt.alpha(root.look.accent, 0.22)
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            x: 1.5 * root.look.unit
            text: slider.label
            color: root.look.label
            font.pixelSize: root.look.labelSize
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 1.5 * root.look.unit
            text: slider.shown
            color: root.look.text
            font.pixelSize: root.look.labelSize
            font.features: { "tnum": 1 }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onPressed: mouse => { slider.forceActiveFocus(); slider.moved(slider.valueAt(mouse.x)) }
            onPositionChanged: mouse => { if (pressed) slider.moved(slider.valueAt(mouse.x)) }
            onWheel: wheel => slider.moved(slider.value + (wheel.angleDelta.y > 0 ? 1 : -1) * slider.step * (wheel.modifiers & Qt.ShiftModifier ? 10 : 5))
        }
        Keys.onPressed: event => {
            const amount = slider.step * (event.modifiers & Qt.ShiftModifier ? 10 : 1)
            if (event.key === Qt.Key_Left || event.key === Qt.Key_Down) slider.moved(slider.value - amount)
            else if (event.key === Qt.Key_Right || event.key === Qt.Key_Up) slider.moved(slider.value + amount)
            else return
            event.accepted = true
        }
        ToolTip.visible: hover.hovered && slider.tip !== ""
        ToolTip.text: slider.tip
        ToolTip.delay: 600
        HoverHandler { id: hover }
    }

    // ---- the picture, the mask over it and the pointer -------------------------------------------

    Item {
        id: board
        anchors.fill: parent

        CompareView {
            id: pictureViewer
            objectName: "maskViewer"
            anchors.fill: parent
            look: root.look
            mode: "single"
            sourceA: root.imageSource
            sizeA: root.imageSize
            labelA: ""
            showZoom: true
        }
        MaskLayer {
            id: paintLayer
            objectName: "maskLayer"
            anchors.fill: parent
            clip: true
            originX: pictureViewer.imageX
            originY: pictureViewer.imageY
            zoom: pictureViewer.imageScale
            tint: Qt.alpha(root.look.accent, 0.5)
            maskOnly: root.maskOnly
        }

        // The lasso's outline and the rectangle, in view coordinates, while a drag is on.
        Shape {
            anchors.fill: parent
            visible: pen.action === "lasso" && pen.screenPath.length > 1
            ShapePath {
                strokeColor: Qt.rgba(0, 0, 0, 0.7)
                strokeWidth: 3
                fillColor: "transparent"
                PathPolyline { path: pen.screenPath }
            }
            ShapePath {
                strokeColor: "white"
                strokeWidth: 1
                fillColor: Qt.alpha(root.look.accent, 0.2)
                PathPolyline { path: pen.screenPath }
            }
        }
        Rectangle {
            objectName: "maskRectPreview"
            visible: pen.action === "rect"
            x: Math.min(pen.pressAt.x, pen.at.x)
            y: Math.min(pen.pressAt.y, pen.at.y)
            width: Math.abs(pen.at.x - pen.pressAt.x)
            height: Math.abs(pen.at.y - pen.pressAt.y)
            color: Qt.alpha(root.look.accent, 0.2)
            border.width: 1
            border.color: "white"
        }

        // The brush: its circle, and the hardness as an inner ring. Drawn dark under white so it reads
        // on any picture.
        Item {
            id: ring
            objectName: "maskBrushRing"
            readonly property bool shown: pen.over && !root.spaceHeld && (root.tool === "brush" || root.tool === "eraser")
                                          && pen.action !== "pan"
            readonly property real diameter: Math.max(2, root.brushSize * pictureViewer.imageScale)
            readonly property point center: pen.action === "resize" ? pen.pressAt : pen.at
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
            Circle { across: ring.diameter; edge: root.erasing ? "#ff9f9a" : "white" }
            Circle {
                visible: root.brushHardness < 0.97 && ring.diameter * root.brushHardness > 6
                across: ring.diameter * root.brushHardness
                edge: Qt.rgba(1, 1, 1, 0.55)
            }
            Rectangle { x: -1; y: -1; width: 3; height: 3; radius: 1.5; color: "white" }
        }

        MouseArea {
            id: pen
            objectName: "maskPointer"
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton // the middle button pans
            preventStealing: true
            cursorShape: root.spaceHeld ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
                         : root.tool === "brush" || root.tool === "eraser" ? Qt.BlankCursor : Qt.CrossCursor

            property bool over: containsMouse
            property point at: Qt.point(0, 0)        // the pointer, in view coordinates
            property point pressAt: Qt.point(0, 0)   // where the press was
            property point last: Qt.point(0, 0)
            property string action: ""               // stroke, lasso, rect, pan, resize ("" when no drag)
            property var path: []                    // the lasso's points, in picture pixels
            property var screenPath: []              // the same, in view coordinates
            property real startSize: 64
            property real startHardness: 0.8
            property bool erase: false

            // A tablet's pressure scales the brush; a mouse is full.
            PointHandler {
                id: stylus
                acceptedDevices: PointerDevice.Stylus
            }
            readonly property real pressure: stylus.active ? Math.max(0.05, stylus.point.pressure) : 1

            // View coordinates to picture pixels.
            function pixelAt(x, y) {
                const f = pictureViewer.pointAt(x, y)
                return Qt.point(f.x * root.imageSize.width, f.y * root.imageSize.height)
            }

            onPressed: mouse => {
                pictureViewer.forceActiveFocus()
                at = Qt.point(mouse.x, mouse.y)
                pressAt = at
                last = at
                const p = pixelAt(mouse.x, mouse.y)
                if (root.spaceHeld || mouse.button === Qt.MiddleButton) {
                    action = "pan"
                } else if ((mouse.modifiers & Qt.AltModifier) && (root.tool === "brush" || root.tool === "eraser")) {
                    action = "resize"
                    startSize = root.brushSize
                    startHardness = root.brushHardness
                } else if (root.tool === "brush" || root.tool === "eraser") {
                    action = "stroke"
                    paintLayer.beginStroke(root.erasing, root.brushSize, root.brushHardness, p.x, p.y, pressure)
                } else if (root.tool === "lasso") {
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
                    paintLayer.strokeTo(p.x, p.y, pressure)
                } else if (action === "lasso") {
                    const prior = screenPath[screenPath.length - 1]
                    if (Math.abs(prior.x - at.x) + Math.abs(prior.y - at.y) >= 2) {
                        path = path.concat([p])
                        screenPath = screenPath.concat([Qt.point(at.x, at.y)])
                    }
                } else if (action === "resize") {
                    // Right grows and up hardens, as ComfyUI's Alt+drag does; the ring follows the pointer's edge.
                    root.setSize(startSize + 2 * (at.x - pressAt.x) / pictureViewer.imageScale)
                    root.setHardness(startHardness + (pressAt.y - at.y) / 200)
                } else if (action === "pan") {
                    if (!pictureViewer.actual && pictureViewer.zoom <= 1)
                        return // at Fit the picture stays put
                    pictureViewer.setView({centerX: pictureViewer.centerX - (at.x - last.x) / (pictureViewer.aw * pictureViewer.imageScale),
                                    centerY: pictureViewer.centerY - (at.y - last.y) / (pictureViewer.ah * pictureViewer.imageScale)})
                    last = at
                }
            }
            // The wheel zooms at the pointer, as the viewer's does (this area is over it).
            onWheel: wheel => pictureViewer.zoomAt(wheel.x, wheel.y, Math.pow(1.15, wheel.angleDelta.y / 120))
            onReleased: mouse => finish(mouse.modifiers)
            onCanceled: finish(Qt.NoModifier)
            onContainsMouseChanged: if (!containsMouse && !pressed) action = ""

            function finish(modifiers) {
                const erasing = (modifiers & Qt.AltModifier) !== 0
                if (action === "stroke") {
                    paintLayer.endStroke()
                } else if (action === "lasso") {
                    if (path.length >= 3)
                        paintLayer.fillPolygon(path, erasing)
                } else if (action === "rect") {
                    const a = pixelAt(pressAt.x, pressAt.y), b = pixelAt(at.x, at.y)
                    if (Math.abs(b.x - a.x) >= 1 && Math.abs(b.y - a.y) >= 1)
                        paintLayer.fillRect(Math.min(a.x, b.x), Math.min(a.y, b.y), Math.abs(b.x - a.x), Math.abs(b.y - a.y), erasing)
                }
                action = ""
                path = []
                screenPath = []
            }
        }
    }

    // ---- the footer: the keys' reminder, then Cancel and OK ---------------------------------------

    footer: Item {
        anchors.left: parent.left
        anchors.right: parent.right
        height: okButton.height
        Text {
            objectName: "maskHint"
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.right: buttons.left
            anchors.rightMargin: 2 * root.look.unit
            elide: Text.ElideRight
            textFormat: Text.PlainText
            color: root.error !== "" ? root.look.error : root.look.label
            font.pixelSize: root.look.captionSize
            text: root.error !== "" ? root.error
                  : qsTr("[ ] size · Shift+[ ] hardness · Alt+drag size and hardness · Space+drag pans · H mask alone")
        }
        Row {
            id: buttons
            anchors.right: parent.right
            spacing: 2 * root.look.unit
            ViewButton {
                objectName: "maskCancel"
                look: root.look
                text: qsTr("Cancel")
                onTapped: root.cancel()
            }
            ViewButton {
                id: okButton
                objectName: "maskOk"
                look: root.look
                text: qsTr("OK")
                primary: true
                onTapped: root.accept()
            }
        }
    }
}
