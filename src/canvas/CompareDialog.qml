pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue

// The large view of a Compare Images node (#62): the same viewer on the full files, in a dialog
// over the window. It shares the node's state through the model, so a mode picked here is the
// node's, and the keys (flip, mode, fit, 100%) act on it as on the node's face. `openFor(node)`
// opens it; Escape or the close button closes it.
Popup {
    id: root
    objectName: "compareDialog"
    required property CanvasModel canvasModel
    readonly property CanvasLook look: App.theme.look
    property string nodeId
    readonly property alias viewer: compareViewer

    function openFor(node) {
        nodeId = node
        refresh()
        open()
    }
    function refresh() {
        compareViewer.compare = canvasModel.compareResult(nodeId)
        compareViewer.viewState = canvasModel.compareView(nodeId)
    }

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.round(parent.width * 0.9)
    height: Math.round(parent.height * 0.9)
    padding: 2 * look.unit
    modal: true
    focus: true
    enter: null
    exit: null
    closePolicy: Popup.CloseOnEscape
    background: PopupSurface { look: root.look }

    Connections {
        target: root.canvasModel
        enabled: root.visible
        function onCompareViewChanged(node) { if (node === root.nodeId) root.refresh() }
        function onPreviewSizeChanged(node) { if (node === root.nodeId) root.refresh() }
    }

    contentItem: Column {
        spacing: 2 * root.look.unit
        Row {
            id: bar
            objectName: "compareBar"
            spacing: root.look.unit
            readonly property real h: 7 * root.look.unit // 28px: above the 24px floor
            Repeater {
                model: [{id: "slider", text: qsTr("Slider")}, {id: "slider_horizontal", text: qsTr("Slider, horizontal")},
                        {id: "side_by_side", text: qsTr("Side by side")}, {id: "flip", text: qsTr("Flip")},
                        {id: "difference", text: qsTr("Difference")}]
                delegate: Rectangle {
                    id: modeButton
                    required property var modelData
                    objectName: "compareMode-" + modelData.id
                    readonly property bool current: compareViewer.mode === modelData.id
                    width: label.implicitWidth + 4 * root.look.unit
                    height: bar.h
                    radius: root.look.smallCorner
                    color: current ? root.look.accent : root.look.well
                    activeFocusOnTab: true
                    Text {
                        id: label
                        anchors.centerIn: parent
                        text: modeButton.modelData.text
                        color: modeButton.current ? root.look.accentForeground : root.look.text
                        font.pixelSize: root.look.labelSize
                    }
                    TapHandler { onTapped: compareViewer.setView({mode: modeButton.modelData.id}) }
                    Keys.onReturnPressed: compareViewer.setView({mode: modelData.id})
                    Keys.onSpacePressed: compareViewer.setView({mode: modelData.id})
                }
            }
            Item { width: 2 * root.look.unit; height: 1 } // a gap of its own between the groups
            Repeater {
                model: [{id: "fit", text: qsTr("Fit")}, {id: "actual", text: qsTr("100%")}]
                delegate: Rectangle {
                    id: zoomButton
                    required property var modelData
                    objectName: "compareZoom-" + modelData.id
                    readonly property bool current: modelData.id === "actual" ? compareViewer.actual : (!compareViewer.actual && compareViewer.zoom === 1)
                    width: Math.max(zoomLabel.implicitWidth + 4 * root.look.unit, bar.h)
                    height: bar.h
                    radius: root.look.smallCorner
                    color: current ? root.look.selectedRow : root.look.well
                    activeFocusOnTab: true
                    Text {
                        id: zoomLabel
                        anchors.centerIn: parent
                        text: zoomButton.modelData.text
                        color: root.look.text
                        font.pixelSize: root.look.labelSize
                    }
                    TapHandler { onTapped: zoomButton.modelData.id === "fit" ? compareViewer.fit() : compareViewer.actualSize() }
                }
            }
            Item { width: 2 * root.look.unit; height: 1 }
            Rectangle {
                objectName: "compareClose"
                width: bar.h
                height: bar.h
                radius: root.look.smallCorner
                color: root.look.well
                activeFocusOnTab: true
                Text { anchors.centerIn: parent; text: "×"; color: root.look.text; font.pixelSize: root.look.textSize }
                TapHandler { onTapped: root.close() }
                Keys.onReturnPressed: root.close()
            }
        }
        CompareNodeView {
            id: compareViewer
            canvasModel: root.canvasModel
            nodeId: root.nodeId
            look: root.look
            full: true
            width: parent.width
            height: parent.height - bar.height - parent.spacing
        }
    }
}
