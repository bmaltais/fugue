import QtQuick
import Fugue

// A Crop node's picture with its frame (#73): CropView fed from the node's numbers (the model's
// cropRect) and the pixel size of the picture arriving at its input, writing a dragged frame back
// as the node's x, y, width and height (one undo step per drag). The glue; the drawing is CropView
// and the geometry CropGeometry, which know nothing of nodes. With one of the four numbers linked
// the frame is not drawn (its value is not known before a run).
CropView {
    id: root
    required property CanvasModel canvasModel
    required property string nodeId
    property var values: canvasModel.cropRect(nodeId)

    pixels: canvasModel.previewSize(nodeId)
    editable: values.editable !== false
    rect: values.editable === false || pixels.width <= 0 ? null : CropGeometry.applied(values, pixels)

    Connections {
        target: root.canvasModel
        function onEdited() { root.values = root.canvasModel.cropRect(root.nodeId) }
        function onPreviewSizeChanged(node) { if (node === root.nodeId) root.pixels = root.canvasModel.previewSize(root.nodeId) }
    }
    onCommitted: r => canvasModel.setCropRect(nodeId, r)
}
