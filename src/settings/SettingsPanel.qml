pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Fugue
import "../canvas"

// The settings panel (#51), opened by app.settings (Ctrl+,): eight pages on the left, the
// page on the right. Each change goes to the model (settings_model.h), which checks it and
// writes the file at once; a refused change shows why under the page and writes nothing.
// Styled as the `?` overlay (docs/canvas-look.md 13): scrim, popup card, level 3 shadow.
Popup {
    id: panel
    objectName: "settingsPanel"
    required property SettingsModel model

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    property int page: 0
    // Why the last change was refused, until the next change.
    property string error: ""

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(880, (parent ? parent.width : 880) - 12 * u)
    height: Math.min(600, (parent ? parent.height : 600) - 12 * u)
    Overlay.modal: Rectangle { color: panel.look.scrim }
    background: PopupSurface { look: panel.look }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; to: 0; duration: 90; easing.type: Easing.OutCubic }
    }
    onAboutToShow: { error = ""; model.reload() }

    // Runs a model call and keeps its refusal, if any.
    function apply(result) { error = result; return result === "" }

    readonly property var pages: [qsTr("General"), qsTr("Folders"), qsTr("Run"), qsTr("Providers"),
                                  qsTr("Keymap"), qsTr("Theme"), qsTr("Packs"), qsTr("Cache")]

    // The desktop portal's folder picker (Qt's own where there is none).
    property var pickDone: null
    function pickFolder(callback) {
        pickDone = callback
        // A line starting with "(" would continue the line above as a call of `callback`.
        const dialog = folderDialog.createObject(panel) as FolderDialog
        dialog.open()
    }
    Component {
        id: folderDialog
        FolderDialog {
            id: d
            objectName: "settingsFolderDialog"
            onAccepted: { if (panel.pickDone) panel.pickDone(d.selectedFolder); d.destroy() }
            onRejected: d.destroy()
        }
    }

    // --- the panel ---------------------------------------------------------------------

    contentItem: ColumnLayout {
        spacing: 0
        LayoutMirroring.enabled: panel.rightToLeft
        LayoutMirroring.childrenInherit: true

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // The pages.
            ColumnLayout {
                Layout.preferredWidth: 44 * panel.u
                Layout.fillWidth: false
                Layout.fillHeight: true
                Layout.margins: 4 * panel.u
                spacing: 0
                Text {
                    Layout.bottomMargin: 4 * panel.u
                    Layout.leftMargin: 2 * panel.u
                    text: qsTr("Settings")
                    color: panel.look.text
                    font.pixelSize: panel.look.overlayTitleSize
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: panel.pages
                    delegate: Rectangle {
                        id: tab
                        required property string modelData
                        required property int index
                        objectName: "settingsPage-" + index
                        Layout.fillWidth: true
                        implicitHeight: 7 * panel.u
                        radius: panel.look.smallCorner
                        color: panel.page === index ? panel.look.selectedRow : tabHover.hovered ? panel.look.well : "transparent"
                        activeFocusOnTab: true
                        Accessible.role: Accessible.PageTab
                        Accessible.name: modelData
                        Accessible.selected: panel.page === index
                        Keys.onSpacePressed: panel.page = index
                        Keys.onUpPressed: if (index > 0) panel.page = index - 1
                        Keys.onDownPressed: if (index < panel.pages.length - 1) panel.page = index + 1
                        HoverHandler { id: tabHover }
                        TapHandler { onTapped: { panel.page = tab.index; panel.error = "" } }
                        Text {
                            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 2 * panel.u }
                            text: tab.modelData
                            color: panel.look.text
                            font.pixelSize: panel.look.textSize
                            font.weight: panel.page === tab.index ? Font.DemiBold : Font.Normal
                        }
                    }
                }
                Item { Layout.fillHeight: true }
            }
            Rectangle { Layout.fillHeight: true; implicitWidth: 1; color: panel.look.hairline }

            // The page.
            Flickable {
                id: scroller
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: width
                contentHeight: pageColumn.implicitHeight + 12 * panel.u
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                ColumnLayout {
                    id: pageColumn
                    x: 6 * panel.u
                    y: 6 * panel.u
                    width: scroller.width - 12 * panel.u
                    spacing: 6 * panel.u

                    // settings.json cannot be used as it is.
                    Repeater {
                        model: panel.page < 3 ? panel.model.problems : []
                        delegate: Text {
                            required property string modelData
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: panel.look.error
                            font.pixelSize: panel.look.labelSize
                            text: modelData
                        }
                    }

                    // General
                    SettingsParts.Setting {
                        visible: panel.page === 0
                        title: qsTr("Canvas navigation")
                        note: panel.model.navigation === "legacy"
                              ? qsTr("Left-drag on empty canvas pans and the wheel zooms; Ctrl+drag selects. ComfyUI's default.")
                              : qsTr("Left-drag on empty canvas selects and the wheel pans; Ctrl+wheel zooms.")
                        Choices {
                            Layout.fillWidth: true
                            name: "navigation"
                            current: panel.model.navigation
                            options: [{value: "legacy", text: qsTr("Legacy")}, {value: "standard", text: qsTr("Standard")}]
                            onPicked: value => panel.apply(panel.model.set("navigation", value))
                        }
                    }
                    SettingsParts.Setting {
                        id: corners
                        readonly property int radius: panel.model.cornerRadius // -1: follow Omarchy
                        visible: panel.page === 0
                        title: qsTr("Corner radius")
                        note: radius < 0 ? qsTr("Nodes, menus and popups follow Omarchy's window rounding.")
                                         : qsTr("Nodes, menus and popups are rounded by this much.")
                        Flow {
                            Layout.fillWidth: true
                            spacing: 2 * panel.u
                            Choices {
                                name: "corner"
                                current: corners.radius < 0 ? "omarchy" : "fixed"
                                options: [{value: "fixed", text: qsTr("Fixed")}, {value: "omarchy", text: qsTr("Follow Omarchy")}]
                                onPicked: value => panel.apply(panel.model.set("corner_radius",
                                    value === "omarchy" ? "omarchy" : corners.radius >= 0 ? corners.radius : 8))
                            }
                            Row {
                                visible: corners.radius >= 0
                                spacing: panel.u
                                SettingsParts.SmallButton {
                                    objectName: "cornerLess"
                                    text: "\u2212"
                                    Accessible.name: qsTr("Less round")
                                    enabled: corners.radius > 0
                                    opacity: enabled ? 1 : 0.5
                                    onClicked: panel.apply(panel.model.set("corner_radius", corners.radius - 1))
                                }
                                Text {
                                    objectName: "cornerValue"
                                    width: 10 * panel.u
                                    anchors.verticalCenter: parent.verticalCenter
                                    horizontalAlignment: Text.AlignHCenter
                                    text: qsTr("%1 px").arg(corners.radius)
                                    color: panel.look.text
                                    font.pixelSize: panel.look.labelSize
                                    font.features: { "tnum": 1 }
                                }
                                SettingsParts.SmallButton {
                                    objectName: "cornerMore"
                                    text: "+"
                                    Accessible.name: qsTr("Rounder")
                                    enabled: corners.radius < panel.model.maxCornerRadius
                                    opacity: enabled ? 1 : 0.5
                                    onClicked: panel.apply(panel.model.set("corner_radius", corners.radius + 1))
                                }
                            }
                        }
                    }
                    SettingsParts.Setting {
                        visible: panel.page === 0
                        title: qsTr("Magnify small nodes on hover")
                        note: qsTr("Zoomed far out, a short hover over a node shows it at full size over itself, so a wire can start there. Off by default.")
                        SettingsParts.Toggle {
                            objectName: "hoverMagnifier"
                            text: on ? qsTr("On") : qsTr("Off")
                            on: panel.model.hoverMagnifier
                            onToggled: value => panel.apply(panel.model.set("hover_magnifier", value))
                        }
                    }
                    SettingsParts.Setting {
                        visible: panel.page === 0
                        title: qsTr("Language")
                        note: qsTr("Follows Omarchy's language unless you pick one here.")
                        Choices {
                            Layout.fillWidth: true
                            name: "language"
                            current: panel.model.language
                            options: [{value: "", text: qsTr("Follow Omarchy")}].concat(
                                         panel.model.languages.map(l => ({value: l.code, text: l.name})))
                            onPicked: value => panel.apply(panel.model.set("language", value))
                        }
                    }

                    // Folders
                    SettingsParts.FolderSetting {
                        visible: panel.page === 1
                        title: qsTr("Output folder")
                        note: qsTr("Where Save Image writes. A Save node's own folder is inside it.")
                        host: panel
                        key: "output_folder"
                        value: panel.model.outputFolder
                        isDefault: panel.model.outputFolderIsDefault
                    }
                    SettingsParts.FolderSetting {
                        visible: panel.page === 1
                        title: qsTr("Inputs folder")
                        note: qsTr("Where images pasted or dropped into Load Image are kept. By default inside the output folder.")
                        host: panel
                        key: "inputs_folder"
                        value: panel.model.inputsFolder
                        isDefault: panel.model.inputsFolderIsDefault
                    }
                    PathsSetting {
                        visible: panel.page === 1
                        title: qsTr("Extra model folders")
                        note: qsTr("Searched for models besides Fugue's own: a ComfyUI models folder as it is, or a folder of one kind of model, such as LoRAs.")
                        host: panel
                        key: "model_paths"
                        value: panel.model.modelPaths
                        kinds: true
                    }
                    PathsSetting {
                        visible: panel.page === 1
                        title: qsTr("Extra pack folders")
                        note: qsTr("Searched for node packs besides Fugue's own.")
                        host: panel
                        key: "pack_paths"
                        value: panel.model.packPaths
                    }

                    // Run
                    SettingsParts.Setting {
                        visible: panel.page === 2
                        title: qsTr("Seeds of new nodes")
                        note: qsTr("Randomize makes a new picture each run; fixed starts on a random number that stays.")
                        Choices {
                            Layout.fillWidth: true
                            name: "newSeed"
                            current: panel.model.newSeed
                            options: [{value: "random", text: qsTr("Randomize")}, {value: "fixed", text: qsTr("Fixed")}]
                            onPicked: value => panel.apply(panel.model.set("new_seed", value))
                        }
                    }
                    SettingsParts.Setting {
                        visible: panel.page === 2
                        title: qsTr("Embed the workflow in new Save Image nodes")
                        note: qsTr("Dropping an image back onto Fugue reloads its workflow. Local file paths are never embedded.")
                        Choices {
                            Layout.fillWidth: true
                            name: "embed"
                            current: panel.model.embedWorkflow
                            options: [{value: "full", text: qsTr("Full")}, {value: "no-prompts", text: qsTr("Without prompts")},
                                      {value: "none", text: qsTr("None")}]
                            onPicked: value => panel.apply(panel.model.set("embed_workflow", value))
                        }
                    }

                    // Providers
                    ProvidersPage {
                        visible: panel.page === 3
                        Layout.fillWidth: true
                        host: panel
                    }

                    // Packs
                    PacksPage {
                        visible: panel.page === 6
                        Layout.fillWidth: true
                        host: panel
                    }

                    // Cache (#141): made when first shown, so the page costs nothing until then.
                    Loader {
                        active: panel.page === 7 || item !== null
                        visible: panel.page === 7
                        Layout.fillWidth: true
                        sourceComponent: CachePage { host: panel }
                    }

                    // Keymap
                    SettingsParts.Setting {
                        visible: panel.page === 4
                        title: qsTr("Keymap file")
                        note: qsTr("Your changes to the default keys, as a list of keys added and keys unset. Press ? for every key.")
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2 * panel.u
                            Text {
                                objectName: "keymapPath"
                                text: App.commands.keymapFile
                                color: panel.look.text
                                font.pixelSize: panel.look.textSize
                            }
                            Row {
                                spacing: panel.u
                                SettingsParts.SmallButton {
                                    objectName: "keymapOpen"
                                    text: qsTr("Open")
                                    onClicked: panel.model.openKeymap()
                                }
                                SettingsParts.SmallButton {
                                    objectName: "keymapReload"
                                    text: qsTr("Reload")
                                    onClicked: panel.model.reloadKeymap()
                                }
                            }
                            Text {
                                visible: App.commands.problems.length === 0
                                text: qsTr("No problems.")
                                color: panel.look.label
                                font.pixelSize: panel.look.labelSize
                            }
                            Repeater {
                                model: App.commands.problems
                                delegate: Text {
                                    required property string modelData
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    color: panel.look.error
                                    font.pixelSize: panel.look.labelSize
                                    text: modelData
                                }
                            }
                        }
                    }

                    // Theme
                    SettingsParts.Setting {
                        visible: panel.page === 5
                        title: qsTr("Theme")
                        note: qsTr("Fugue follows the Omarchy theme and the desktop text size. Change them in Omarchy. The canvas corners are set on the General page.")
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 4 * panel.u
                            rowSpacing: panel.u
                            Text { text: qsTr("Mode"); color: panel.look.label; font.pixelSize: panel.look.labelSize }
                            Text {
                                text: App.theme.light ? qsTr("Light") : qsTr("Dark")
                                color: panel.look.text
                                font.pixelSize: panel.look.textSize
                            }
                            Text { text: qsTr("Window rounding"); color: panel.look.label; font.pixelSize: panel.look.labelSize }
                            Text { text: App.theme.radius; color: panel.look.text; font.pixelSize: panel.look.textSize }
                            Text { text: qsTr("Text size"); color: panel.look.label; font.pixelSize: panel.look.labelSize }
                            Text { text: App.theme.textSize; color: panel.look.text; font.pixelSize: panel.look.textSize }
                            Text { text: qsTr("Colors"); color: panel.look.label; font.pixelSize: panel.look.labelSize }
                            Flow {
                                Layout.fillWidth: true
                                spacing: panel.u
                                Repeater {
                                    model: Object.keys(App.theme.colors)
                                    delegate: Rectangle {
                                        required property string modelData
                                        width: 5 * panel.u
                                        height: 5 * panel.u
                                        radius: panel.look.smallCorner
                                        color: App.theme.colors[modelData]
                                        border.width: 1
                                        border.color: panel.look.hairline
                                        Accessible.name: modelData
                                        ToolTip.visible: swatchHover.hovered
                                        ToolTip.text: modelData
                                        HoverHandler { id: swatchHover }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // The refusal of the last change, and where the settings live.
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: panel.look.hairline }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 3 * panel.u
            Layout.leftMargin: 6 * panel.u
            Layout.rightMargin: 6 * panel.u
            Text {
                objectName: "settingsError"
                Layout.fillWidth: true
                visible: panel.error !== ""
                text: panel.error
                wrapMode: Text.Wrap
                color: panel.look.error
                font.pixelSize: panel.look.labelSize
            }
            Text {
                Layout.fillWidth: true
                visible: panel.error === ""
                elide: Text.ElideMiddle
                // Where the page's changes go; the keymap and theme pages say it themselves.
                text: panel.page === 3 ? qsTr("Saved to %1 as you change it.").arg(panel.model.providersPath)
                    : panel.page < 3 ? qsTr("Saved to %1 as you change it.").arg(panel.model.path) : ""
                color: panel.look.label
                font.pixelSize: panel.look.captionSize
            }
            Text {
                text: qsTr("Esc to close")
                color: panel.look.label
                font.pixelSize: panel.look.captionSize
            }
        }
    }
}
