pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// The apps shelf where a person lands with nothing open (#388, #463, docs/ui-ergonomics.md "Apps shelf"): in the canvas's place, a
// kit panel (AppShelfView) with the installed apps as cards, the search, Start blank for the person who came to build a graph, and
// Browse templates… for the rest of the catalog. It sits in the window as the App view's cards do: a gap of the window's background
// round it. Made by Main.qml only while AppShelf.atHome, so a window started on a file never builds it. Choosing a card opens the
// app; the window then has something open and this goes away by itself.
Item {
    id: home
    objectName: "appsHome"

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property real gap: look.params.areaLayout === "panes" ? 0 : look.params.areaGap * u

    // Read when it is made, and again when the catalog's models change (a download ended, a host answered).
    Component.onCompleted: {
        App.workflows.loadHostModels()
        App.appShelf.refresh()
        view.focusSearch()
    }
    Connections {
        target: App.workflows
        function onTemplatesChanged() { App.appShelf.refresh() }
    }
    Connections {
        target: App.workflows.downloads
        function onFinished() { App.appShelf.refresh() }
    }

    AppShelfView {
        id: view
        anchors { fill: parent; margins: home.gap }
        look: home.look
        onChosen: id => App.appShelf.choose(id)
    }
}
