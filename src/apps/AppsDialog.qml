pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// File > Apps… (#388): the same shelf as the home page, as a dialog over whatever is open, for the person who has tabs and wants
// an app. Styled as the Templates dialog (scrim, popup card, level 3 shadow); Escape closes. Made by Main.qml the first time
// app.show-all asks for it.
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
    background: PopupSurface { look: dialog.look }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; to: 0; duration: 90; easing.type: Easing.OutCubic }
    }
    // Read each time it opens: models and folders change outside Fugue.
    onAboutToShow: {
        App.workflows.loadHostModels()
        App.appShelf.refresh()
    }
    onOpened: shelf.focusCards()
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

    contentItem: ColumnLayout {
        spacing: 3 * dialog.u
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 4 * dialog.u
            Layout.bottomMargin: 0
            Text {
                Layout.fillWidth: true
                text: qsTr("Apps")
                color: dialog.look.text
                font.pixelSize: dialog.look.overlayTitleSize
                font.weight: Font.DemiBold
            }
            ViewButton {
                objectName: "appsDialogBrowse"
                look: dialog.look
                text: qsTr("Browse templates")
                glyph: "layout-template"
                bordered: true
                tip: qsTr("All the templates, apps and workflows")
                onTapped: {
                    dialog.close()
                    App.commands.trigger("file.templates")
                }
            }
        }
        AppShelfGrid {
            id: shelf
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 2 * dialog.u
            onChosen: id => {
                if (App.appShelf.choose(id))
                    dialog.close()
            }
        }
    }
}
