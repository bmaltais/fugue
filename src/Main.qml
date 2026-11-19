import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "canvas"
import "menu"
import "search"
import "settings"
import "workflow"

ApplicationWindow {
    id: win
    width: 960
    // 680 for the canvas and the menu bar's 7u at the default text size.
    height: 708
    minimumWidth: 640
    minimumHeight: 460
    visible: true
    title: workflows.windowTitle
    color: theme.background
    font.pixelSize: theme.textSize

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    // The settings (settings/settings_model.h); a test window may run without them.
    readonly property var appSettingsModel: typeof appSettings === "undefined" ? null : appSettings

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            MenuBar {
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
            }
            WorkflowTabs {
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
            }
            FugueCanvas {
                id: canvas
                objectName: "canvas"
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: canvasModel
                navigation: win.appSettingsModel ? win.appSettingsModel.navigation : "legacy"
                onSearchRequested: (x, y) => canvasModel.search.openAt(x, y)
                onLinkReleased: (from, x, y) => canvasModel.search.openFromLink(from, x, y)
                onContextMenuRequested: (x, y) => contextMenu.openAt(x, y, false)

                // Tab and Shift+A (`node.search`) open search where the pointer is; Add node
                // in the context menu, where it was opened.
                Connections {
                    target: canvasModel.search
                    function onPointerSearchRequested() {
                        const t = commands.target()
                        const p = t.x !== undefined ? Qt.point(t.x, t.y) : canvas.pointerWorld()
                        canvasModel.search.openAt(p.x, p.y)
                    }
                }
                ContextMenu {
                    id: contextMenu
                    anchors.fill: parent
                    canvas: canvas
                }
            }
        }

        // The file pickers, the save and trust and recovery questions, the drop area.
        WorkflowDialogs { anchors.fill: parent; canvasView: canvas }
    }

    // The command registry's shortcuts, the `?` overlay and the quit question.
    CommandUi { anchors.fill: parent }

    // The settings panel (#51), made the first time app.settings (Ctrl+,) asks for it.
    Connections {
        target: commands
        function onWindowRequested(id) {
            if (id !== "app.settings" || !win.appSettingsModel)
                return
            if (!win.settingsPanel)
                win.settingsPanel = settingsComponent.createObject(win.contentItem)
            win.settingsPanel.open()
        }
    }
    property var settingsPanel: null
    Component {
        id: settingsComponent
        SettingsPanel { model: win.appSettingsModel }
    }

    NodeSearchPopup {
        id: searchPopup
        canvas: canvas
    }
}
