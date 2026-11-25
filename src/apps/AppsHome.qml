pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue
import "../canvas"

// The apps shelf where a person lands with nothing open (#388, docs/ui-ergonomics.md "Apps shelf"): in the canvas's place, the
// installed apps as cards, Start blank for the person who came to build a graph, and Browse templates… for the rest of the
// catalog. Made by Main.qml only while AppShelf.atHome, so a window started on a file never builds it. Choosing a card opens
// the app; the window then has something open and this goes away by itself.
Rectangle {
    id: home
    objectName: "appsHome"

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit

    color: look.popup

    // Read when it is made, and again when the catalog's models change (a download ended, a host answered).
    Component.onCompleted: {
        App.workflows.loadHostModels()
        App.appShelf.refresh()
    }
    Connections {
        target: App.workflows
        function onTemplatesChanged() { App.appShelf.refresh() }
    }
    Connections {
        target: App.workflows.downloads
        function onFinished() { App.appShelf.refresh() }
    }

    ColumnLayout {
        anchors { fill: parent; margins: 4 * home.u }
        spacing: 3 * home.u

        RowLayout {
            Layout.fillWidth: true
            spacing: 3 * home.u
            ColumnLayout {
                Layout.fillWidth: true
                spacing: home.u
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Apps")
                    color: home.look.text
                    font.pixelSize: home.look.overlayTitleSize
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Pick one, give it a picture or a few words, press Run.")
                    color: home.look.label
                    font.pixelSize: home.look.labelSize
                    wrapMode: Text.Wrap
                }
            }
            ViewButton {
                objectName: "appsBrowseTemplates"
                look: home.look
                text: qsTr("Browse templates")
                glyph: "layout-template"
                bordered: true
                tip: qsTr("All the templates, apps and workflows")
                onTapped: App.commands.trigger("file.templates")
            }
            ViewButton {
                objectName: "appsStartBlank"
                look: home.look
                text: qsTr("Start blank")
                glyph: "workflow"
                bordered: true
                tip: qsTr("An empty graph, to build a workflow from nothing")
                onTapped: App.appShelf.dismiss()
            }
        }

        AppShelfGrid {
            id: shelf
            Layout.fillWidth: true
            Layout.fillHeight: true
            onChosen: id => App.appShelf.choose(id)
            Component.onCompleted: focusCards()
        }
    }
}
