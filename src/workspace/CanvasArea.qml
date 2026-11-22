pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../canvas"

// The Canvas view in an area (#217 slice 1, spec 12.16): CanvasView.qml with the context the File > Canvas… dialog
// gives it (CanvasViewDialog.qml): the session's store, and as the follow target the one Canvas Region node selected
// in the Graph, which also moves the view to that node's canvas. The canvas it shows is the area's state
// ({"canvasId"}), kept with the layout, so a window opens on the canvas each area last showed.
AreaView {
    id: root
    objectName: "canvasArea"
    readonly property alias view: content
    property var edits: App.canvasModel.widgets
    property var selection: App.canvasModel.selection
    readonly property string selectedRegion: edits && selection.length === 1 && edits.regionState(selection[0]).ok === true ? selection[0] : ""

    CanvasView {
        id: content
        anchors.fill: parent
        look: App.theme.look
        canvasId: root.areaState?.canvasId ?? ""
        edits: root.edits
        followNode: root.selectedRegion
        focus: true
    }
    Component.onCompleted: {
        // A kept canvas that is gone: the newest, as a new view opens on.
        const rows = content.backend.canvases
        if (content.canvasId !== "" && !rows.some(row => row.id === content.canvasId))
            content.canvasId = rows.length > 0 ? rows[0].id : ""
        // What it opened on (the newest, with none kept) is kept once its area listens.
        Qt.callLater(root.keep)
    }
    function keep() {
        if (content.canvasId !== "")
            areaStateEdited({canvasId: content.canvasId})
    }
    Connections {
        target: content.backend
        // The workspace keeps it only when it differs from what it has.
        function onCanvasChanged() { root.keep() }
    }
}
