import QtQuick
import Fugue

// A node's picture with its painted mask over it (#174, spec 14.4): Load Image's own picture, or the
// one arriving at Paint Mask's image input. The picture is the node's usual thumbnail; the mask is a
// MaskLayer over it, read from `maskFile` at no more than 1024 px on a side, the overlay of the editor
// (the accent at half strength). Made only at full detail and only for a node whose mask is painted, so
// a node without one stays a plain thumbnail. It takes no input: FugueCanvas takes every press, the
// double click that opens the editor (Paint Mask) or the large view (Load Image) included.
Item {
    id: root
    required property CanvasModel canvasModel
    required property string nodeId
    required property CanvasLook look
    property alias source: thumbnail.source
    property alias info: thumbnail.info
    property alias text: thumbnail.text
    property string maskFile // the painted mask's file, empty for none
    property bool compact: false // the Compact node card's picture corner (Thumbnail's)
    property size pixels: canvasModel.previewSize(nodeId)
    readonly property alias overlayLayer: overlay

    // The mask is kept this small: the face is a thumbnail, the editor has the full size.
    readonly property real longest: 1024
    readonly property real shrink: Math.min(1, longest / Math.max(1, pixels.width, pixels.height))
    readonly property real fit: pixels.width > 0 ? Math.min(width / pixels.width, height / pixels.height) : 1

    Thumbnail {
        id: thumbnail
        anchors.fill: parent
        look: root.look
        full: true
        compact: root.compact
    }
    MaskLayer {
        id: overlay
        objectName: "maskOverlay"
        anchors.fill: parent
        visible: root.maskFile !== "" && root.pixels.width > 0 && root.text === ""
        imageSize: Qt.size(Math.max(1, Math.round(root.pixels.width * root.shrink)),
                           Math.max(1, Math.round(root.pixels.height * root.shrink)))
        zoom: root.fit / root.shrink
        originX: (width - root.pixels.width * root.fit) / 2
        originY: (height - root.pixels.height * root.fit) / 2
        tint: Qt.alpha(root.look.accent, 0.5)
        function reload() {
            if (root.maskFile !== "" && root.pixels.width > 0)
                loadMask(root.maskFile)
            else
                reset()
        }
        onImageSizeChanged: reload()
        Component.onCompleted: reload()
        Connections {
            target: root
            function onMaskFileChanged() { overlay.reload() }
        }
    }
    Connections {
        target: root.canvasModel
        function onPreviewSizeChanged(node) { if (node === root.nodeId) root.pixels = root.canvasModel.previewSize(root.nodeId) }
    }
}
