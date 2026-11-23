pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../canvas"

// The Canvas view in an area (#217 slice 1, spec 12.16): CanvasView.qml with the context the File > Canvas… dialog
// gives it (CanvasViewDialog.qml): the session's store, the document and the Graph's selection (the view follows the
// selected Canvas Region node, which also moves it to that node's canvas, or finds one itself). The canvas it shows is
// the area's state ({"canvasId"}), kept with the layout, so a window opens on the canvas each area last showed.
AreaView {
    id: root
    objectName: "canvasArea"
    readonly property alias view: content
    property var edits: App.canvasModel.widgets
    property var selection: App.canvasModel.selection

    // The area's own ground, the File > Canvas… dialog's surface color (nothing else draws behind an area), and the
    // dialog's 2u inset, so the view reads the same in both hosts.
    Rectangle {
        anchors.fill: parent
        color: App.theme.look.popup
    }
    CanvasView {
        id: content
        anchors.fill: parent
        anchors.margins: 2 * App.theme.look.unit
        look: App.theme.look
        canvasId: root.areaState?.canvasId ?? ""
        edits: root.edits
        selection: root.selection
        focus: true
        // Its menus are in the area's header (AreaHeader.qml), as the Graph's are, not in the view.
        ownMenus: false
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
