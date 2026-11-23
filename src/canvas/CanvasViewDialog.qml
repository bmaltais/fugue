pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// File > Canvas… (#221, epic #216): the Canvas view (CanvasView.qml) in a large view, for the one-area window (D2). The
// dialog only frames it and gives it its context: the canvas, the document and the Graph's selection (the view finds
// the Canvas Region node to follow from it). One bar (#228): the frame's own bar is off and its close sits
// at the end of the view's tools, after the view's Fit and 100%, as the Mask editor's and the Crop window's bars end.
// `openFor(id)` opens it on a canvas (empty: the newest, or the selected region's); Escape closes.
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
    zoomable: content.viewer.visible ? content.viewer : null
    showBar: false

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
        ownShortcuts: true // a modal popup: the window's shortcuts sleep, so the layers panel binds its keys
        hostCloses: true // File > Close canvas closes this dialog
        onCloseRequested: root.close()
        edits: root.edits
        selection: root.selection
        focus: true
        trailing: ViewButton {
            objectName: "canvasViewClose"
            look: root.look
            glyph: "x" // the Lucide x of every other close (canvas-look 16)
            tip: qsTr("Close (Esc)")
            onTapped: root.close()
        }
    }
}
