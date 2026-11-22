pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// One picture with a crop frame on it (#73): a zoom and pan viewer (CompareView in its single mode)
// with a CropFrame over it. It knows nothing of nodes or the canvas: give it a picture, its pixel
// size and the frame; it announces the frame a person drags out.
//
// Input:  source (the picture; a smaller copy is fine), pixels (the file's size, which the frame is
//         in), rect ({x, y, width, height}; none draws no frame), ratio (0 = free), editable, look,
//         and the viewer's own: `viewer` (zoom, pan, Fit, 100%, `wheelModifiers`, `interactive`).
// Output: committed(rect) at the end of a drag, changing(rect) during it, pressed, activated (a double click), as CropFrame.
Item {
    id: root
    required property CanvasLook look
    property url source
    property size pixels: Qt.size(0, 0)
    property var rect: null
    property real ratio: 0
    property bool editable: true
    property int wheelModifiers: Qt.NoModifier
    readonly property alias viewer: viewer

    signal committed(var rect)
    signal changing(var rect) // at every step of a drag, as CropFrame's
    signal pressed
    signal activated

    CompareView {
        id: viewer
        objectName: "cropViewer"
        anchors.fill: parent
        look: root.look
        mode: "single"
        sourceA: root.source
        sizeA: root.pixels
        labelA: ""
        showZoom: true
        wheelModifiers: root.wheelModifiers
        onPressed: root.pressed()
        onActivated: root.activated()
    }
    Loader {
        anchors.fill: parent
        active: root.rect !== null && root.pixels.width > 0
        sourceComponent: CropFrame {
            id: cropFrame
            look: root.look
            imageSize: root.pixels
            originX: viewer.imageX
            originY: viewer.imageY
            zoom: viewer.imageScale
            rect: root.rect
            ratio: root.ratio
            editable: root.editable
            onCommitted: r => root.committed(r)
            onChanging: r => root.changing(r)
            onPressed: root.pressed()
            onActivated: root.activated()
        }
    }
}
