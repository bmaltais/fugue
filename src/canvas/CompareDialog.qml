pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// The large view of a Compare Images node (#62): the same viewer on the full files, in a dialog
// over the window (LargeViewDialog: the bar, Fit, 100%, close, the keys). It shares the node's
// state through the model, so a mode picked here is the node's, and the keys (flip, mode, fit,
// 100%) act on it as on the node's face. `openFor(node)` opens it; Escape or the close button closes it.
LargeViewDialog {
    id: root
    objectName: "compareDialog"
    name: "compare"
    look: App.theme.look
    required property CanvasModel canvasModel
    property string nodeId
    readonly property alias viewer: compareViewer
    zoomable: compareViewer

    function openFor(node) {
        nodeId = node
        refresh()
        open()
    }
    function refresh() {
        compareViewer.compare = canvasModel.compareResult(nodeId)
        compareViewer.viewState = canvasModel.compareView(nodeId)
    }

    Connections {
        target: root.canvasModel
        enabled: root.visible
        function onCompareViewChanged(node) { if (node === root.nodeId) root.refresh() }
        function onPreviewSizeChanged(node) { if (node === root.nodeId) root.refresh() }
    }

    tools: Repeater {
        model: [{id: "slider", text: qsTr("Slider")}, {id: "slider_horizontal", text: qsTr("Slider, horizontal")},
                {id: "side_by_side", text: qsTr("Side by side")}, {id: "flip", text: qsTr("Flip")},
                {id: "difference", text: qsTr("Difference")}]
        delegate: ViewButton {
            required property var modelData
            objectName: "compareMode-" + modelData.id
            look: root.look
            text: modelData.text
            current: compareViewer.mode === modelData.id
            onTapped: compareViewer.setView({mode: modelData.id})
        }
    }
    CompareNodeView {
        id: compareViewer
        canvasModel: root.canvasModel
        nodeId: root.nodeId
        look: root.look
        full: true
        anchors.fill: parent
    }
}
