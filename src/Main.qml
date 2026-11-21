pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Fugue
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
    title: App.workflows.windowTitle
    color: App.theme.background
    font.pixelSize: App.theme.textSize

    Material.theme: App.theme.light ? Material.Light : Material.Dark
    Material.accent: App.theme.accent
    Material.background: App.theme.background
    Material.foreground: App.theme.foreground

    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    // The settings (settings/settings_model.h); a test window may run without them.
    readonly property SettingsModel appSettingsModel: App.settings

    HelperNotice { id: helperNotice }

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
            // Faster generation on an NVIDIA card with only the Vulkan helper (#97, spec 8.10).
            // Made only while shown: the window keeps few items (#5).
            Loader {
                Layout.fillWidth: true
                Layout.preferredHeight: item ? (item as Item).implicitHeight : 0
                active: helperNotice.shown
                sourceComponent: Rectangle {
                    objectName: "helperNotice"
                    implicitHeight: noticeRow.implicitHeight + 16
                    color: App.theme.look.popup
                    border.color: App.theme.look.hairline
                    RowLayout {
                        id: noticeRow
                        anchors { fill: parent; margins: 8 }
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: App.theme.look.text
                            text: qsTr("Faster generation on your NVIDIA card: install fugue-helper-cuda")
                        }
                        ThemedButton {
                            objectName: "helperNoticeCopy"
                            highlighted: true
                            text: qsTr("Copy %1").arg(helperNotice.command)
                            onClicked: helperNotice.copy()
                        }
                        ToolButton {
                            objectName: "helperNoticeDismiss"
                            text: "×"
                            focusPolicy: Qt.NoFocus
                            Accessible.name: qsTr("Dismiss")
                            onClicked: helperNotice.dismiss()
                        }
                    }
                }
            }
            FugueCanvas {
                id: fugueCanvas
                objectName: "canvas"
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: App.canvasModel
                navigation: win.appSettingsModel ? win.appSettingsModel.navigation : "legacy"
                onSearchRequested: (x, y) => App.canvasModel.search.openAt(x, y)
                onLinkReleased: (from, x, y) => App.canvasModel.search.openFromLink(from, x, y)
                onContextMenuRequested: (x, y) => contextMenu.openAt(x, y, false)
                onMenuRequested: (kind, x, y) => contextMenu.openMenu(kind, x, y)

                // Tab and Shift+A (`node.search`) open search where the pointer is; Add node
                // in the context menu, where it was opened.
                Connections {
                    target: App.canvasModel.search
                    function onPointerSearchRequested() {
                        const t = App.commands.target()
                        const p = t.x !== undefined ? Qt.point(t.x, t.y) : fugueCanvas.pointerWorld()
                        App.canvasModel.search.openAt(p.x, p.y)
                    }
                }
                ContextMenu {
                    id: contextMenu
                    anchors.fill: parent
                    canvas: fugueCanvas
                }
            }
        }

        // The file pickers, the save and trust and recovery questions, the drop area.
        WorkflowDialogs { anchors.fill: parent; canvasView: fugueCanvas }
        GroupDialogs { anchors.fill: parent }
        LayoutColorPicker {}
        ImageSaveDialog {}
    }

    // The command registry's shortcuts, the `?` overlay and the quit question.
    CommandUi { anchors.fill: parent }

    // The settings panel (#51), made the first time app.settings (Ctrl+,) asks for it.
    Connections {
        target: App.commands
        function onWindowRequested(id) {
            if (id === "file.templates") {
                if (!win.templatesDialog)
                    win.templatesDialog = templatesComponent.createObject(win.contentItem)
                win.templatesDialog.open()
                return
            }
            if (id !== "app.settings" || !win.appSettingsModel)
                return
            if (!win.settingsPanel)
                win.settingsPanel = settingsComponent.createObject(win.contentItem)
            win.settingsPanel.open()
        }
    }
    property var settingsPanel: null
    // The banner's Install for a workflow's missing pack (#91): Settings > Packs, which shows
    // what it is and asks before it installs and before it is trusted.
    Connections {
        target: App.workflows
        function onPackInstallRequested(source, ref) {
            if (!win.appSettingsModel)
                return
            if (!win.settingsPanel)
                win.settingsPanel = settingsComponent.createObject(win.contentItem)
            win.settingsPanel.page = win.settingsPanel.pages.length - 1 // Packs, the last page
            win.settingsPanel.open()
            win.appSettingsModel.packs.add(source, ref)
        }
    }
    // The Templates dialog (#83), made the first time file.templates asks for it.
    property var templatesDialog: null
    Component {
        id: templatesComponent
        TemplatesDialog {}
    }
    Component {
        id: settingsComponent
        SettingsPanel { model: win.appSettingsModel }
    }

    NodeSearchPopup {
        id: searchPopup
        canvas: fugueCanvas
    }
}
