pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// A crop frame over a picture (#73), drawn and edited on top of any viewer. It knows nothing of the
// viewer, a node or the canvas: give it the picture's pixel size, where the picture's corner is in
// this item's coordinates and how many view units one pixel is (`zoom`) (the viewer's pan and zoom), and the
// frame in pixels; it draws the frame with the area outside dimmed, a rule-of-thirds grid while a
// drag is going on, eight handles and the pixel size, and edits it by dragging inside it (move) or
// by a handle (resize). The geometry is CropGeometry (engine/crop_geometry.h), tested apart.
//
// Input:  look, imageSize, originX, originY, zoom, rect ({x, y, width, height} pixels), ratio
//         (width / height; 0 = free), editable.
// Output: committed(rect) when a drag ends with a different frame (the caller writes it: one
//         undo step per drag), changing(rect) at every step of a drag, pressed on a press that
//         lands on the frame, activated on a double click on it.
// Keys while dragging: Shift keeps the frame's ratio (the lock, when there is one, always holds),
//         Alt resizes from the center (Photoshop's).
// A press outside the frame, or any press when not `editable`, is not taken: the viewer under it
// pans.
Item {
    id: frame
    required property CanvasLook look
    property size imageSize: Qt.size(0, 0)
    property real originX: 0
    property real originY: 0
    property real zoom: 1
    property var rect: ({x: 0, y: 0, width: 1, height: 1})
    property real ratio: 0
    property bool editable: true

    signal committed(var rect)
    signal changing(var rect)
    signal pressed
    signal activated

    readonly property bool dragging: pointer.dragging
    // The frame drawn: the drag's, while there is one.
    readonly property var shown: pointer.dragging ? pointer.draft : rect
    readonly property real grab: 12 // screen px: a 24px press target on every handle (the floor)
    readonly property real handleSize: 8
    clip: true

    readonly property real fx: originX + shown.x * zoom
    readonly property real fy: originY + shown.y * zoom
    readonly property real fw: shown.width * zoom
    readonly property real fh: shown.height * zoom
    readonly property real iw: imageSize.width * zoom
    readonly property real ih: imageSize.height * zoom
    readonly property color dim: Qt.rgba(0, 0, 0, 0.55)

    // The dimmed area: the four strips of the picture around the frame.
    Rectangle { objectName: "cropDimTop"; x: frame.originX; y: frame.originY; width: frame.iw; height: Math.max(0, frame.fy - frame.originY); color: frame.dim }
    Rectangle { objectName: "cropDimBottom"; x: frame.originX; y: frame.fy + frame.fh; width: frame.iw; height: Math.max(0, frame.originY + frame.ih - frame.fy - frame.fh); color: frame.dim }
    Rectangle { objectName: "cropDimLeft"; x: frame.originX; y: frame.fy; width: Math.max(0, frame.fx - frame.originX); height: frame.fh; color: frame.dim }
    Rectangle { objectName: "cropDimRight"; x: frame.fx + frame.fw; y: frame.fy; width: Math.max(0, frame.originX + frame.iw - frame.fx - frame.fw); height: frame.fh; color: frame.dim }

    // The frame: a white line on a dark one, so it reads on any picture.
    Rectangle {
        objectName: "cropFrame"
        x: frame.fx - 1
        y: frame.fy - 1
        width: frame.fw + 2
        height: frame.fh + 2
        color: "transparent"
        border.width: 1
        border.color: Qt.rgba(0, 0, 0, 0.6)
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            color: "transparent"
            border.width: 1
            border.color: "white"
        }
    }
    // The rule of thirds, while a drag is going on.
    Repeater {
        model: pointer.dragging ? [1, 2] : []
        delegate: Item {
            id: thirds
            required property int modelData
            Rectangle { x: frame.fx + frame.fw * thirds.modelData / 3; y: frame.fy; width: 1; height: frame.fh; color: Qt.rgba(1, 1, 1, 0.5) }
            Rectangle { x: frame.fx; y: frame.fy + frame.fh * thirds.modelData / 3; width: frame.fw; height: 1; color: Qt.rgba(1, 1, 1, 0.5) }
        }
    }
    // The handles: corners always, edge midpoints when the side is long enough to hold two.
    Repeater {
        model: frame.editable ? [{h: "top-left", u: 0, v: 0}, {h: "top-right", u: 1, v: 0}, {h: "bottom-left", u: 0, v: 1},
                                 {h: "bottom-right", u: 1, v: 1}, {h: "top", u: 0.5, v: 0}, {h: "bottom", u: 0.5, v: 1},
                                 {h: "left", u: 0, v: 0.5}, {h: "right", u: 1, v: 0.5}] : []
        delegate: Rectangle {
            id: handle
            required property var modelData
            objectName: "cropHandle-" + modelData.h
            visible: (modelData.u === 0.5 ? frame.fw : modelData.v === 0.5 ? frame.fh : 1e9) >= 3 * frame.handleSize
            x: frame.fx + frame.fw * modelData.u - width / 2
            y: frame.fy + frame.fh * modelData.v - height / 2
            width: frame.handleSize
            height: frame.handleSize
            color: "white"
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, 0.7)
        }
    }
    // The size while a drag is going on, in a chip inside the frame's top left (or the picture's).
    Rectangle {
        objectName: "cropSizeChip"
        visible: pointer.dragging
        x: Math.max(0, Math.min(frame.fx + frame.look.unit, frame.width - width))
        y: Math.max(0, Math.min(frame.fy + frame.look.unit, frame.height - height))
        width: sizeText.implicitWidth + 2 * frame.look.unit
        height: sizeText.implicitHeight + frame.look.unit
        radius: frame.look.smallCorner
        color: Qt.rgba(0, 0, 0, 0.7)
        Text {
            id: sizeText
            anchors.centerIn: parent
            text: qsTr("%1 × %2").arg(frame.shown.width).arg(frame.shown.height)
            color: "white"
            font.pixelSize: frame.look.captionSize
            font.features: { "tnum": 1 }
        }
    }

    MouseArea {
        id: pointer
        objectName: "cropMouse"
        anchors.fill: parent
        enabled: frame.editable
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: cursorOf(pressed ? handle : hovered)

        property bool dragging: false
        property var draft: frame.rect
        property string handle: "none"   // the handle being dragged
        property string hovered: "none"  // the handle under the pointer
        property point start
        property var startRect: frame.rect
        property real lockRatio: 0

        function cursorOf(h) {
            switch (h) {
            case "top-left": case "bottom-right": return Qt.SizeFDiagCursor
            case "top-right": case "bottom-left": return Qt.SizeBDiagCursor
            case "top": case "bottom": return Qt.SizeVerCursor
            case "left": case "right": return Qt.SizeHorCursor
            case "inside": return Qt.SizeAllCursor
            default: return Qt.ArrowCursor
            }
        }
        function under(x, y) {
            return CropGeometry.handleAt(frame.rect, (x - frame.originX) / frame.zoom, (y - frame.originY) / frame.zoom,
                                         frame.grab / frame.zoom)
        }
        function offset(mouse) {
            return Qt.point(Math.round((mouse.x - start.x) / frame.zoom), Math.round((mouse.y - start.y) / frame.zoom))
        }
        function follow(mouse) {
            const d = offset(mouse)
            let r
            if (handle === "inside") {
                r = CropGeometry.moved(startRect, d.x, d.y, frame.imageSize)
            } else {
                // The lock holds always; Shift keeps the ratio the frame had when the drag began.
                const ratio = frame.ratio > 0 ? frame.ratio
                              : (mouse.modifiers & Qt.ShiftModifier) ? startRect.width / startRect.height : 0
                r = CropGeometry.resized(startRect, handle, d.x, d.y, frame.imageSize, ratio, !!(mouse.modifiers & Qt.AltModifier))
            }
            draft = r
            frame.changing(r)
        }
        function same(a, b) { return a.x === b.x && a.y === b.y && a.width === b.width && a.height === b.height }

        onPressed: mouse => {
            const h = under(mouse.x, mouse.y)
            if (h === "none") {
                mouse.accepted = false // the viewer under it pans
                return
            }
            handle = h
            start = Qt.point(mouse.x, mouse.y)
            startRect = frame.rect
            draft = frame.rect
            dragging = true
            frame.pressed()
        }
        onPositionChanged: mouse => {
            if (pressed && dragging)
                follow(mouse)
            else
                hovered = under(mouse.x, mouse.y)
        }
        onReleased: mouse => {
            if (!dragging)
                return
            follow(mouse)
            dragging = false
            if (!same(draft, frame.rect))
                frame.committed(draft)
        }
        onCanceled: dragging = false
        onDoubleClicked: frame.activated()
        onExited: hovered = "none"
    }
}
