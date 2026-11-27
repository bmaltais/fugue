pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// Two images, A and B, compared in one frame. It knows nothing of the canvas, a node or a run:
// give it two image sources, their sizes and a mode, and it draws them; every change a person
// makes (the divider, the zoom, a flip) is in `view` and announced by `viewChanged`, so the
// caller keeps it and can hand it back (the node's face and the large view share one).
//
// Input:  sourceA, sourceB, sourceDifference (the |A - B| picture; empty hides that mode's
//         picture), sizeA, sizeB (the pixel sizes of the files, which the sources may be smaller
//         copies of), labelA, labelB, look, and the view below.
// Modes:  "slider" (a divider, A one side, B the other; the default and the primary), "slider_horizontal",
//         "side_by_side", "flip" (A over B, click to flip) and "difference".
// View:   mode, position (the divider, 0..1), showB (flip), zoom (1 = fit, up to 64), actual (one
//         image pixel to one screen pixel, which beats zoom), actualB (that picture is B, the larger one: #420), centerX and centerY (the point of the
//         image at the middle, 0..1). Zoom and pan are one transform for both images (linked).
//         Pixels are shown as they are (nearest-neighbour) once enlarged past the file's own.
// "single" is one picture, A alone, for the large view of any node's image (#73): a drag pans (once
// zoomed), no divider, no B, and `showZoom` adds the zoom to the size notice. It is not one of `modes`
// the keys cycle through.
// B is drawn in A's frame, cropped to A's shape and centered (the same fit as the Compare Images
// node's difference), and a notice says so when the sizes differ.
// `placeB`: where B sits inside A, a rect in A's pixels (an App that grows a picture knows where the input went, the
// block's `compare`). Set, the slider modes and flip draw B at that rect over A's whole frame, so the two line up, and the
// notice about sizes is not said. Side by side and difference ignore it (they have no shared frame to place B in). Empty (the
// default, and the graph's Compare node and dialogs): B is fitted to A's frame as above. Zoom and pan move both together.
Item {
    id: view
    required property CanvasLook look
    property url sourceA
    property url sourceB
    property url sourceDifference
    // The files themselves, when `sources` are smaller copies of them: drawn over the copy, once loaded, while `detail` is
    // on (the node's face asks for it when zoomed in, #371). Left empty, the view only ever draws its sources.
    property url fileA
    property url fileB
    property url fileDifference
    property bool detail: false
    property size sizeA: Qt.size(0, 0)
    property size sizeB: Qt.size(0, 0)
    property string labelA: "A"
    property string labelB: "B"
    property rect placeB: Qt.rect(0, 0, 0, 0)

    property string mode: "slider"
    property real position: 0.5
    property bool showB: false
    property real zoom: 1
    property bool actual: false
    property bool actualB: false   // 100% of B (when B is the larger picture), else of A
    property real centerX: 0.5
    property real centerY: 0.5

    // Behavior of the caller's choosing.
    property bool interactive: true
    property bool hoverWipe: true  // the divider follows the pointer, as well as being dragged
    property bool chrome: true     // the labels and the size notice
    property bool showZoom: false  // the zoom in percent after the size notice
    property bool zoomChips: false // Fit and 100% on the face while the pointer is over it (#420)
    property bool showSize: true   // the size notice when the sizes agree (the Canvas view says it in its status line); a mismatch is always said
    property int wheelModifiers: Qt.NoModifier // held to zoom with the wheel (the node's face asks for Alt)
    property color background: look.well
    // A rounded frame (the Compact node card, #476): the corners are cut in `cornerColor`, the colour behind the view. 0: square, as
    // the large views. On the software renderer, which has no shaders, the corners stay square.
    property real cornerRadius: 0
    property color cornerColor: "transparent"

    readonly property var modes: ["slider", "slider_horizontal", "side_by_side", "flip", "difference"]
    readonly property bool single: mode === "single"
    readonly property bool sliding: mode === "slider" || mode === "slider_horizontal"
    readonly property bool placed: placeB.width > 0 && placeB.height > 0
    readonly property bool differs: !placed && sizeA.width > 0 && sizeB.width > 0
                                    && (sizeA.width !== sizeB.width || sizeA.height !== sizeB.height)
    // One image pixel is `imageScale` screen pixels, so the zoom in percent is that.
    // 100% is of B: B is larger, and the view asked for it (CompareDetail, engine/compare_scale.h).
    // B is the larger picture: by area as the node's Compare is; placed, by pixel density (B's pixels per A pixel over its rect: the
    // input of an expanded result is as large as the result's own pixels, or larger, never "larger" by being only part of it).
    readonly property real densityB: placed ? sizeB.width / placeB.width : 1
    readonly property bool bLarger: placed ? sizeB.width > 0 && sizeA.width > 0 && densityB > 1 : CompareDetail.bIsLarger(sizeA, sizeB)
    readonly property bool actualOfB: actual && actualB && bLarger
    readonly property string zoomText: qsTr("%1%").arg(Math.round(imageScale * 100))
    readonly property string sizeNotice: (differs || placed) && actual && bLarger
        ? (actualOfB ? qsTr("100% of B · %1 × %2").arg(sizeB.width).arg(sizeB.height)
                     : qsTr("100% of A · %1 × %2").arg(sizeA.width).arg(sizeA.height))
        : differs
        ? qsTr("A %1 × %2, B %3 × %4: B fitted to A").arg(sizeA.width).arg(sizeA.height).arg(sizeB.width).arg(sizeB.height)
        : sizeA.width > 0 ? (showZoom ? qsTr("%1 × %2 · %3").arg(sizeA.width).arg(sizeA.height).arg(zoomText)
                                      : qsTr("%1 × %2").arg(sizeA.width).arg(sizeA.height)) : ""

    // The whole state a person changes.
    readonly property var current: ({mode: mode, position: position, showB: showB, zoom: zoom,
                                     actual: actual, actualB: actualB, centerX: centerX, centerY: centerY})
    signal viewChanged(var current)
    signal pressed // a press on the picture (the node's face selects the node)
    signal activated // a double click on it (the node's face opens the large view)

    function setView(changes) {
        if (changes.mode !== undefined) mode = changes.mode
        if (changes.position !== undefined) position = Math.max(0, Math.min(1, changes.position))
        if (changes.showB !== undefined) showB = changes.showB
        if (changes.zoom !== undefined) zoom = Math.max(1, Math.min(64, changes.zoom))
        if (changes.actual !== undefined) actual = changes.actual
        if (changes.actualB !== undefined) actualB = changes.actualB
        if (changes.centerX !== undefined) centerX = Math.max(0, Math.min(1, changes.centerX))
        if (changes.centerY !== undefined) centerY = Math.max(0, Math.min(1, changes.centerY))
        viewChanged(current)
    }
    // Takes a whole view (the caller's state) without announcing it.
    function apply(v) {
        if (v.mode !== undefined) mode = v.mode
        if (v.position !== undefined) position = v.position
        if (v.showB !== undefined) showB = v.showB
        if (v.zoom !== undefined) zoom = v.zoom
        if (v.actual !== undefined) actual = v.actual
        if (v.actualB !== undefined) actualB = v.actualB
        if (v.centerX !== undefined) centerX = v.centerX
        if (v.centerY !== undefined) centerY = v.centerY
    }
    function nextMode() { if (!single) setView({mode: modes[(modes.indexOf(mode) + 1) % modes.length]}) }
    // Flip: A over B. From another mode it enters the flip mode at A; in it, it shows the other.
    function flip() { if (!single) setView(mode === "flip" ? {showB: !showB} : {mode: "flip", showB: false}) }
    function fit() { setView({zoom: 1, actual: false, actualB: false, centerX: 0.5, centerY: 0.5}) }
    // 100% of the larger picture; pressed again with the sizes differing, of the other (#420).
    function actualSize() {
        setView(placed ? {actual: true, actualB: bLarger && (actual ? !actualB : true)}
                       : CompareDetail.nextActual(sizeA, sizeB, actual, actualB))
    }
    // Zooms by `factor` keeping the image point under (x, y) of the view where it is.
    function zoomAt(x, y, factor) {
        const before = imageScale
        const target = Math.max(before * factor, fitScale)
        const next = Math.min(target, fitScale * 64)
        const p = pointAt(x, y)
        setView({zoom: next / fitScale, actual: false, actualB: false})
        // Put the image point back under the pointer.
        const o = panelFor(x)
        setView({centerX: p.x - (x - o.cx) / (aw * imageScale), centerY: p.y - (y - o.cy) / (ah * imageScale)})
    }
    // The picture of the view as an image, for Copy view: callback(QImage-ish result).
    function grab(done) { content.grabToImage(done) }

    // ---- geometry -----------------------------------------------------------------------

    readonly property real aw: Math.max(1, sizeA.width > 0 ? sizeA.width : 1)
    readonly property real ah: Math.max(1, sizeA.height > 0 ? sizeA.height : 1)
    readonly property bool paired: mode === "side_by_side"
    // A panel is the region one image fills: all of the view, or half of it side by side.
    readonly property real panelW: paired ? width / 2 : width
    readonly property real fitScale: Math.min(panelW / aw, height / ah)
    readonly property real imageScale: actual ? (placed ? (actualB && bLarger ? densityB : 1) : CompareDetail.actualScale(sizeA, sizeB, actualB))
                                               : fitScale * zoom
    // The panel of the image under x: its center on screen.
    function panelFor(x) {
        const right = paired && x >= width / 2
        return {cx: panelW / 2 + (right ? panelW : 0), cy: height / 2, right: right}
    }
    // Screen x of the image's left edge in the panel centered at cx, and y of the top.
    function edgeX(cx) { return cx - centerX * aw * imageScale }
    function edgeY() { return height / 2 - centerY * ah * imageScale }
    // Where the image's top left corner is, in the view's coordinates (single and the slider modes:
    // one panel), for whatever draws over the picture (the crop frame).
    readonly property real imageX: edgeX(panelW / 2)
    readonly property real imageY: edgeY()
    // The image point (0..1) at the screen point.
    function pointAt(x, y) {
        const o = panelFor(x)
        return Qt.point(centerX + (x - o.cx) / (aw * imageScale), centerY + (y - o.cy) / (ah * imageScale))
    }

    // ---- drawing ------------------------------------------------------------------------

    Item {
        id: content
        anchors.fill: parent
        clip: true

        Rectangle { anchors.fill: parent; color: view.background }
        ShaderEffect {
            objectName: "viewCorners"
            z: 10
            visible: view.cornerRadius > 0 && GraphicsInfo.api !== GraphicsInfo.Software
            anchors.fill: parent
            readonly property size size: Qt.size(width, height)
            readonly property real radius: view.cornerRadius
            readonly property color cornerColor: view.cornerColor
            fragmentShader: "qrc:/canvas/shaders/corners.frag.qsb"
        }

        // The image of a Layer: A's frame, smooth down to the file's own pixels, then nearest-neighbour.
        component Picture: Image {
            required property real panelCx
            required property point origin // the layer's top left
            required property rect place   // where the picture sits in A's pixels; empty: A's whole frame
            readonly property bool placedIn: place.width > 0 && place.height > 0
            x: view.edgeX(panelCx) - origin.x + (placedIn ? place.x * view.imageScale : 0)
            y: view.edgeY() - origin.y + (placedIn ? place.y * view.imageScale : 0)
            width: (placedIn ? place.width : view.aw) * view.imageScale
            height: (placedIn ? place.height : view.ah) * view.imageScale
            asynchronous: true
            cache: false
            fillMode: Image.PreserveAspectCrop
            smooth: implicitWidth <= 0 || width <= implicitWidth
            mipmap: smooth
        }

        // One image, drawn in the frame of A inside the clip rectangle (x, y, w, h of the view). With `sharpSource` and
        // the view's `detail` on, the file is loaded beside the copy and takes over when it is ready: the copy stays
        // drawn until then, so nothing flashes, and goes (with the file) when `detail` goes off.
        component Layer: Item {
            id: piece
            property url source
            property url sharpSource
            property real panelCx: view.panelW / 2
            property rect place: Qt.rect(0, 0, 0, 0)
            clip: true
            Picture {
                place: piece.place
                panelCx: piece.panelCx
                origin: Qt.point(piece.x, piece.y)
                source: piece.source
                visible: !sharp.ready
            }
            Picture {
                id: sharp
                place: piece.place
                panelCx: piece.panelCx
                origin: Qt.point(piece.x, piece.y)
                readonly property bool ready: status === Image.Ready
                source: view.detail ? piece.sharpSource : ""
                visible: ready
            }
        }

        // The slider modes and flip: B under, A over (or the other way for flip).
        Layer {
            objectName: "layerB"
            visible: view.sliding || (view.mode === "flip" && view.showB)
            place: view.placeB
            source: view.sourceB
            sharpSource: view.fileB
            width: view.width
            height: view.height
        }
        Layer {
            objectName: "layerA"
            visible: view.sliding || view.single || (view.mode === "flip" && !view.showB)
            source: view.sourceA
            sharpSource: view.fileA
            width: view.mode === "slider" ? view.position * view.width : view.width
            height: view.mode === "slider_horizontal" ? view.position * view.height : view.height
        }
        // Side by side: A in the left half, B in the right, one transform.
        Layer {
            objectName: "layerPairA"
            visible: view.paired
            source: view.sourceA
            sharpSource: view.fileA
            width: view.panelW
            height: view.height
        }
        Layer {
            objectName: "layerPairB"
            visible: view.paired
            x: view.panelW
            panelCx: view.panelW + view.panelW / 2
            source: view.sourceB
            sharpSource: view.fileB
            width: view.panelW
            height: view.height
        }
        Layer {
            objectName: "layerDifference"
            visible: view.mode === "difference"
            source: view.sourceDifference
            sharpSource: view.fileDifference
            width: view.width
            height: view.height
        }
        // The divider: a line with a grip, 2 px, the accent on a dark edge so it reads on any picture.
        Item {
            objectName: "divider"
            visible: view.sliding
            readonly property bool vertical: view.mode === "slider"
            x: vertical ? Math.round(view.position * view.width) - 1 : 0
            y: vertical ? 0 : Math.round(view.position * view.height) - 1
            width: vertical ? 2 : view.width
            height: vertical ? view.height : 2
            Rectangle { anchors.fill: parent; color: "white" }
            Rectangle {
                anchors.centerIn: parent
                width: parent.vertical ? 2 + 2 * view.look.unit : 2 * view.look.unit + 2
                height: parent.vertical ? 2 * view.look.unit + 2 : 2 + 2 * view.look.unit
                radius: view.look.smallCorner
                color: Qt.rgba(0, 0, 0, 0.6)
                border.width: 1
                border.color: "white"
            }
        }
        Rectangle { // between the panels
            visible: view.paired
            x: view.panelW - 0.5
            width: 1
            height: view.height
            color: Qt.rgba(1, 1, 1, 0.5)
        }
    }

    // ---- labels and the size notice -------------------------------------------------------

    component Chip: Rectangle {
        id: chip
        property alias text: label.text
        implicitWidth: label.implicitWidth + 2 * view.look.unit
        implicitHeight: label.implicitHeight + view.look.unit
        width: Math.min(implicitWidth, view.width / 2 - 2 * view.look.unit)
        height: implicitHeight
        radius: view.look.smallCorner
        color: Qt.rgba(0, 0, 0, 0.6)
        visible: view.chrome && text !== ""
        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            x: view.look.unit
            width: chip.width - 2 * view.look.unit
            elide: Text.ElideMiddle
            textFormat: Text.PlainText
            color: "white"
            font.pixelSize: view.look.captionSize
            font.features: { "tnum": 1 }
        }
    }
    // The first and the second picture's labels, placed by the mode.
    Chip {
        objectName: "labelA"
        x: view.look.unit
        y: view.look.unit
        text: view.mode === "difference" ? qsTr("|A − B|")
              : view.mode === "flip" && view.showB ? view.labelB : view.labelA
    }
    Chip {
        objectName: "labelB"
        visible: view.chrome && (view.sliding || view.paired)
        x: view.width - width - view.look.unit
        y: view.mode === "slider_horizontal" ? view.height - height - view.look.unit : view.look.unit
        text: view.labelB
    }
    Chip {
        objectName: "sizeNotice"
        x: view.look.unit
        y: view.height - height - view.look.unit
        text: view.showSize || view.differs ? view.sizeNotice : ""
        color: view.differs ? Qt.rgba(0.55, 0.3, 0, 0.85) : Qt.rgba(0, 0, 0, 0.6)
    }

    // Fit and 100% on the face (#420), over the pointer's picture so a press on them is theirs, not the divider's.
    // Only while the pointer is over the view, and not on a face too small to hold them beside the labels.
    HoverHandler { id: hover; enabled: view.interactive }
    // The image point (0..1 of A's frame) under the pointer, or (-1, -1) when the pointer is not over the view.
    function pointerImagePoint() {
        if (!hover.hovered)
            return Qt.point(-1, -1)
        return pointAt(hover.point.position.x, hover.point.position.y)
    }
    Row {
        objectName: "zoomChips"
        visible: view.zoomChips && view.chrome && hover.hovered && view.width >= 24 * view.look.unit
        anchors.right: parent.right
        anchors.rightMargin: view.look.unit
        anchors.bottom: parent.bottom
        anchors.bottomMargin: view.look.unit
        spacing: view.look.unit / 2
        z: 1
        Repeater {
            model: [{id: "fit", text: qsTr("Fit")}, {id: "actual", text: qsTr("100%")}]
            delegate: Chip {
                id: zoomChip
                required property var modelData
                objectName: "zoomChip-" + modelData.id
                readonly property bool on: modelData.id === "fit" ? !view.actual && view.zoom === 1 : view.actual
                text: modelData.text
                visible: true
                color: on ? Qt.rgba(0.1, 0.35, 0.65, 0.85) : Qt.rgba(0, 0, 0, 0.6)
                Accessible.role: Accessible.Button
                Accessible.name: text
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: zoomChip.modelData.id === "fit" ? view.fit() : view.actualSize()
                }
            }
        }
    }

    // The arrow keys move the divider, 1% a press and 10% with Shift (img-comparison-slider's
    // keyboard). Home is not the divider's: it fits the picture, as in every picture view and the
    // Graph (docs/ui-ergonomics.md D5), so End is not either.
    activeFocusOnTab: interactive
    Keys.onPressed: event => {
        const step = (event.modifiers & Qt.ShiftModifier) ? 0.1 : 0.01
        if (!sliding)
            return
        const back = mode === "slider" ? Qt.Key_Left : Qt.Key_Up
        const forward = mode === "slider" ? Qt.Key_Right : Qt.Key_Down
        if (event.key === back) setView({position: position - step})
        else if (event.key === forward) setView({position: position + step})
        else return
        event.accepted = true
    }

    // ---- pointer ------------------------------------------------------------------------

    MouseArea {
        id: pointer
        objectName: "compareMouse"
        anchors.fill: parent
        enabled: view.interactive
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        cursorShape: view.sliding && pressed === false ? (view.mode === "slider" ? Qt.SizeHorCursor : Qt.SizeVerCursor)
                     : pressed ? Qt.ClosedHandCursor : Qt.ArrowCursor
        property point last
        property bool moved: false
        // Pans instead of moving the divider: a middle press, or Shift held.
        property bool panning: false

        function wipeTo(x, y) {
            view.setView({position: view.mode === "slider" ? x / view.width : y / view.height})
        }
        onPressed: mouse => {
            view.forceActiveFocus()
            view.pressed()
            last = Qt.point(mouse.x, mouse.y)
            moved = false
            panning = mouse.button === Qt.MiddleButton || (mouse.modifiers & Qt.ShiftModifier) || !view.sliding
            if (!panning)
                wipeTo(mouse.x, mouse.y)
        }
        onPositionChanged: mouse => {
            if (pressed) {
                if (Math.abs(mouse.x - last.x) + Math.abs(mouse.y - last.y) > 3)
                    moved = true
                if (panning) {
                    if (view.single && !view.actual && view.zoom <= 1)
                        return // a single picture at Fit stays put
                    const dx = mouse.x - last.x, dy = mouse.y - last.y
                    last = Qt.point(mouse.x, mouse.y)
                    view.setView({centerX: view.centerX - dx / (view.aw * view.imageScale),
                                  centerY: view.centerY - dy / (view.ah * view.imageScale)})
                } else {
                    wipeTo(mouse.x, mouse.y)
                }
            } else if (view.sliding && view.hoverWipe) {
                wipeTo(mouse.x, mouse.y)
            }
        }
        onClicked: mouse => {
            if (view.mode === "flip" && !moved && mouse.button === Qt.LeftButton)
                view.flip()
        }
        onDoubleClicked: mouse => {
            if (mouse.button === Qt.LeftButton)
                view.activated()
        }
    }
    WheelHandler {
        // Wayland calls an ordinary mouse a TouchPad: take both device types.
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        enabled: view.interactive
        acceptedModifiers: view.wheelModifiers
        onWheel: event => view.zoomAt(point.position.x, point.position.y, Math.pow(1.15, event.angleDelta.y / 120))
    }
}
