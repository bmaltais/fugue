import QtQuick
import Fugue

// The large view of any node's picture (#73): the full file, zoomed at the pointer with the wheel,
// panned by dragging, Fit and 100% (and 0 and 1), the pixel size and the zoom shown, Escape closes.
// `openFor(node)` opens it on the picture the node shows (its last result, else its input file's
// image: CanvasRuns.imageUrl). The frame, bar and keys are LargeViewDialog's; the viewer is
// CompareView in its single mode.
LargeViewDialog {
    id: root
    objectName: "imageDialog"
    name: "image"
    look: App.theme.look
    required property CanvasModel canvasModel
    property string nodeId
    readonly property alias viewer: view
    zoomable: view

    function openFor(node) {
        nodeId = node
        view.sourceA = App.runs ? App.runs.imageUrl(node) : ""
        view.sizeA = canvasModel.previewSize(node)
        view.labelA = canvasModel.nodeTitle(node)
        view.fit()
        open()
    }

    CompareView {
        id: view
        objectName: "imageViewer"
        anchors.fill: parent
        look: root.look
        mode: "single"
        showZoom: true
    }
}
