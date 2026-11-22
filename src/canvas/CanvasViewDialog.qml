pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// File > Canvas… (#221, epic #216): the Canvas view (CanvasView.qml) in a large view, until the workspace's areas
// can host it. The dialog only frames it and gives it its context: the canvas, and as the follow target the one
// Canvas Region node selected in the graph (the selection drives the view). `openFor(id)` opens it on a canvas
// (empty: the newest, or the selected region's); Escape closes.
LargeViewDialog {
    id: root
    objectName: "canvasViewDialog"
    name: "canvasView"
    look: App.theme.look
    readonly property alias view: content
    // A test points the dialog at a store of its own, and at the document whose selection it follows.
    property alias storeRoot: content.storeRoot
    property var edits: App.canvasModel.widgets
    property var selection: App.canvasModel.selection
    readonly property string selectedRegion: edits && selection.length === 1 && edits.regionState(selection[0]).ok === true ? selection[0] : ""
    zoomable: content.viewer.visible ? content.viewer : null

    function openFor(canvasId) {
        if (canvasId !== "")
            content.canvasId = canvasId
        content.refresh()
        open()
    }

    CanvasView {
        id: content
        anchors.fill: parent
        look: root.look
        zoomButtons: false
        edits: root.edits
        followNode: root.selectedRegion
        focus: true
    }
}
