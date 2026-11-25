pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue

// The mask editor (#172, spec 14.4): a picture at full size to paint a mask on. A LargeViewDialog
// whose viewer is CompareView in `single` mode with a MaskLayer over it. It knows nothing of nodes:
// give it the picture and, if there is one, the mask to start from; it says what was saved.
//
// `layers: true` is the paint window of the Paint node (#181) in the same frame: the pointer, brush,
// shapes, zoom, keys and undo are the same code, driving a PaintLayer (the layers drawn as the result
// will be) instead of a MaskLayer, with a layers panel at the right and a color swatch and an opacity
// slider in the bar. Its input is `paintPath` (a stored .ora, or empty), `pictureFile` (the input
// picture's file, empty for a blank canvas) and `imageSize` (the canvas, in pixels); `imageSource` is
// not used. It saves the layers as one .ora file: `saved(path)` says it, or an empty path when nothing
// is painted. A mask layer is painted white, whatever the color.
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
    tone: layers ? "sea" : "ember"

    property url imageSource
    property size imageSize: Qt.size(0, 0)
    property string maskPath
    // The paint window (see above) instead of the mask editor.
    property bool layers: false
    property string paintPath
    property string pictureFile
    property string stem: {
        const file = String(layers ? pictureFile : imageSource).split("?")[0].split("/").pop()
        const base = decodeURIComponent(file).replace(/\.[^.]*$/, "")
        return (base === "" ? (layers ? "paint" : "image") : base) + (layers ? "-paint" : "-mask")
    }
    property string folder
    onFolderChanged: { maskLayer.folder = folder; colorLayer.folder = folder }
    signal saved(string path)
    signal canceled

    // What the pointer does, and the brush: its size in picture pixels, its hardness (0 to 1).
    property string tool: "brush" // brush, eraser, lasso, rect
    property real brushSize: 64
    property real brushHardness: 0.8
    property bool maskOnly: false
    // The paint window's brush color (a mask layer ignores it) and opacity 0 to 1.
    property color color: "#1a1a1a"
    property real brushOpacity: 1
    readonly property real maxBrush: Math.max(64, Math.min(2048, Math.max(imageSize.width, imageSize.height)))
    // The mask editor's layer, and the paint window's; `canvasLayer` is the one in use. Both have the same calls.
    readonly property alias layer: maskLayer
    readonly property alias paintCanvas: colorLayer
    readonly property var canvasLayer: layers ? colorLayer : maskLayer
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
    function setBrushOpacity(v) { brushOpacity = Math.max(0, Math.min(1, Math.round(v * 100) / 100)) }
    function pickColor() { colorPicker.openWith(String(color)) }
    // OK: the mask (or the layers) is stored and its path said; nothing painted is no file, an empty path.
    function accept() {
        error = ""
        let path = ""
        if (!canvasLayer.empty) {
            path = canvasLayer.store(stem)
            if (path === "") {
                error = canvasLayer.lastError() !== "" ? canvasLayer.lastError()
                        : layers ? qsTr("The painting could not be saved.") : qsTr("The mask could not be saved.")
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
        if (layers) {
            colorLayer.imageSize = imageSize
            colorLayer.reset()
            colorLayer.checker = pictureFile === ""
            if (pictureFile !== "" && !colorLayer.setPicture(pictureFile))
                error = colorLayer.lastError()
            else if (paintPath !== "" && !colorLayer.loadPaint(paintPath))
                error = colorLayer.lastError()
            else if (colorLayer.warning() !== "")
                error = colorLayer.warning()
        } else {
            maskLayer.imageSize = imageSize
            maskLayer.reset()
            if (maskPath !== "" && !maskLayer.loadMask(maskPath))
                error = maskLayer.lastError()
        }
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
            if (event.key === Qt.Key_Z && !shift) root.canvasLayer.undo()
            else if ((event.key === Qt.Key_Z && shift) || event.key === Qt.Key_Y) root.canvasLayer.redo()
            else return
        } else if (event.modifiers & (Qt.AltModifier | Qt.MetaModifier)) {
            return
        } else if (event.key === Qt.Key_B) tool = "brush"
        else if (event.key === Qt.Key_E) tool = "eraser"
        else if (event.key === Qt.Key_X) swapTools()
        else if (event.key === Qt.Key_L) tool = "lasso"
        else if (event.key === Qt.Key_R) tool = "rect"
        else if (event.key === Qt.Key_I) root.canvasLayer.invert()
        else if (event.key === Qt.Key_H) toggleMaskOnly()
        else if (event.key === Qt.Key_Delete && shift) root.canvasLayer.clear()
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
        // A one-key tool of the bar. The current tool is the quiet marking: OK is the one accent fill (D7).
        component ToolButton: ViewButton {
            required property string toolName
            look: root.look
            accented: false
            current: root.tool === toolName
            onTapped: root.tool = toolName
        }
        ToolButton { objectName: "maskTool-brush"; toolName: "brush"; glyph: "brush"; tip: qsTr("Brush (B)") }
        ToolButton { objectName: "maskTool-eraser"; toolName: "eraser"; glyph: "eraser"; tip: qsTr("Eraser (E, X swaps)") }
        ToolButton { objectName: "maskTool-lasso"; toolName: "lasso"; glyph: "lasso-select"; tip: qsTr("Lasso (L, Alt erases)") }
        ToolButton { objectName: "maskTool-rect"; toolName: "rect"; glyph: "square-dashed"; tip: qsTr("Rectangle (R, Alt erases)") }
        Item { width: root.look.unit; height: 1 }
        ValueSlider {
            look: root.look
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
            look: root.look
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
        // The paint window's color and opacity: the swatch opens the shared color picker.
        Rectangle {
            objectName: "paintColor"
            visible: root.layers
            width: 7 * root.look.unit
            height: 7 * root.look.unit
            radius: root.look.smallCorner
            color: root.color
            border.width: swatchFocus.activeFocus ? 2 : 1
            border.color: swatchFocus.activeFocus ? root.look.accent : root.look.hairlineStrong
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Brush color")
            FocusScope {
                id: swatchFocus
                anchors.fill: parent
                activeFocusOnTab: parent.visible
                Keys.onReturnPressed: root.pickColor()
                Keys.onSpacePressed: root.pickColor()
            }
            TapHandler { onTapped: root.pickColor() }
            HoverHandler { id: swatchHover; cursorShape: Qt.PointingHandCursor }
            ToolTip.visible: swatchHover.hovered
            ToolTip.delay: root.look.tooltipDelay
            ToolTip.text: qsTr("Brush color (opens the color picker)")
        }
        ValueSlider {
            look: root.look
            objectName: "paintOpacity"
            visible: root.layers
            label: qsTr("Opacity")
            from: 0
            to: 1
            value: root.brushOpacity
            shown: qsTr("%1%").arg(Math.round(root.brushOpacity * 100))
            step: 0.01
            tip: qsTr("Brush opacity")
            onMoved: v => root.setBrushOpacity(v)
        }
        Item { width: root.look.unit; height: 1 }
        ViewButton {
            objectName: "maskUndo"
            look: root.look
            glyph: "undo-2"
            tip: qsTr("Undo (Ctrl+Z)")
            enabled: root.canvasLayer.canUndo
            onTapped: root.canvasLayer.undo()
        }
        ViewButton {
            objectName: "maskRedo"
            look: root.look
            glyph: "redo-2"
            tip: qsTr("Redo (Ctrl+Shift+Z)")
            enabled: root.canvasLayer.canRedo
            onTapped: root.canvasLayer.redo()
        }
        Item { width: root.look.unit; height: 1 }
        ViewButton {
            objectName: "maskInvert"
            look: root.look
            glyph: "contrast"
            tip: qsTr("Invert (I)")
            onTapped: root.canvasLayer.invert()
        }
        ViewButton {
            objectName: "maskFill"
            look: root.look
            glyph: "paint-bucket"
            tip: root.layers ? qsTr("Fill the layer with the color") : qsTr("Fill the whole picture")
            onTapped: root.canvasLayer.fill()
        }
        ViewButton {
            objectName: "maskOnly"
            look: root.look
            glyph: "eye"
            tip: root.layers ? qsTr("Show the mask layers alone (H)") : qsTr("Show the mask alone (H)")
            accented: false // on is the quiet marking (D7)
            checkable: true
            current: root.maskOnly
            onTapped: root.toggleMaskOnly()
        }
        // The destructive one, apart from the rest of the tools by twice the gap, and undoable.
        Item { width: root.look.unit; height: 1 }
        ViewButton {
            objectName: "maskClear"
            look: root.look
            glyph: "trash-2"
            tip: root.layers ? qsTr("Clear the layer (Shift+Delete)") : qsTr("Clear the mask (Shift+Delete)")
            danger: true
            onTapped: root.canvasLayer.clear()
        }
    }

    // ---- the picture, the mask over it and the pointer -------------------------------------------

    Item {
        id: board
        anchors.fill: parent
        anchors.rightMargin: root.layers ? layersPanel.width + 2 * root.look.unit : 0

        CompareView {
            id: pictureViewer
            objectName: "maskViewer"
            anchors.fill: parent
            look: root.look
            mode: "single"
            sourceA: root.layers ? "" : root.imageSource
            sizeA: root.imageSize
            labelA: ""
            showZoom: true
        }
        MaskLayer {
            id: maskLayer
            objectName: "maskLayer"
            anchors.fill: parent
            visible: !root.layers
            clip: true
            originX: pictureViewer.imageX
            originY: pictureViewer.imageY
            zoom: pictureViewer.imageScale
            tint: Qt.alpha(root.look.accent, 0.5)
            maskOnly: root.maskOnly
        }
        PaintLayer {
            id: colorLayer
            objectName: "paintLayer"
            anchors.fill: parent
            visible: root.layers
            clip: true
            originX: pictureViewer.imageX
            originY: pictureViewer.imageY
            zoom: pictureViewer.imageScale
            tint: Qt.alpha(root.look.accent, 0.5)
            maskOnly: root.maskOnly
            color: root.color
            brushOpacity: root.brushOpacity
        }

        // The pointer: brush, eraser, lasso and rectangle, Alt+drag sizing, pan and zoom (BrushPen.qml, the Canvas
        // view's mask brush too).
        BrushPen {
            id: pen
            anchors.fill: parent
            look: root.look
            viewer: pictureViewer
            target: root.canvasLayer
            imageSize: root.imageSize
            tool: root.tool
            brushSize: root.brushSize
            brushHardness: root.brushHardness
            spaceHeld: root.spaceHeld
            onPressedAt: pictureViewer.forceActiveFocus()
            onResize: (size, hardness) => { root.setSize(size); root.setHardness(hardness) }
        }
    }

    PaintLayersPanel {
        id: layersPanel
        visible: root.layers
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        look: root.look
        painting: colorLayer
    }
    ColorPickerPopup {
        id: colorPicker
        onPicked: c => root.color = c
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
                  : root.layers && !root.canvasLayer.canPaint ? qsTr("The picture is not painted on: pick a paint or mask layer.")
                  : root.layers ? qsTr("[ ] size · Shift+[ ] hardness · Alt+drag size and hardness · Space+drag pans · H mask layers alone")
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
