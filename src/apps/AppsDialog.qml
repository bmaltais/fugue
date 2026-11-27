pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "../kit"

// File > Apps… (#388, #463): the same shelf as the home page (AppShelfView), as a dialog over whatever is open, for the person who
// has tabs and wants an app. A kit panel with a level 3 shadow; the focus opens on the search field, Escape clears it and then
// closes, and the header's close button does too. Made by Main.qml the first time app.show-all asks for it.
Popup {
    id: dialog
    objectName: "appsDialog"

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(1000, (parent ? parent.width : 960) - 12 * u)
    height: Math.min(700, (parent ? parent.height : 640) - 12 * u)
    Overlay.modal: Rectangle { color: dialog.look.scrim }
    background: Item {
        Shadow { target: box; level: 3; look: dialog.look }
        Item { id: box; anchors.fill: parent }
    }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: dialog.look.params.durColor * 1.4; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: dialog.look.params.durColor * 1.4; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; to: 0; duration: dialog.look.params.durColor * 0.9; easing.type: Easing.OutCubic }
    }
    KitFocusKeep { popup: dialog }
    // Read each time it opens: models and folders change outside Fugue.
    onAboutToShow: {
        App.workflows.loadHostModels()
        App.appShelf.refresh()
        shelf.clearSearch()
    }
    onOpened: shelf.focusSearch()
    Connections {
        target: App.workflows
        enabled: dialog.visible
        function onTemplatesChanged() { App.appShelf.refresh() }
    }
    Connections {
        target: App.workflows.downloads
        enabled: dialog.visible
        function onFinished() { App.appShelf.refresh() }
    }
    // A copy made here opens too, and the dialog goes with it.
    Connections {
        target: App.appShelf
        enabled: dialog.visible
        function onChosen() { dialog.close() }
    }

    contentItem: AppShelfView {
        id: shelf
        look: dialog.look
        asDialog: true
        onCloseRequested: dialog.close()
        onChosen: id => App.appShelf.choose(id)
    }
}
