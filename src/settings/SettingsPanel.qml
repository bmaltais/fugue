pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../kit"
import "../menu"
import "../canvas"

// The settings panel (#51), opened by app.settings (Ctrl+,): nine pages on the left, the
// page on the right. Each change goes to the model (settings_model.h), which checks it and
// writes the file at once; a refused change shows why under the page and writes nothing.
// In the Drift kit (docs/canvas-look.md 20, #464): a Slate KitPanel with a KitHero header, the pages as tone-tinted rows on the left
// (as the Templates categories), each page's settings as kit cards and rows on the right. Keys: opening focuses the current page, Tab
// and Shift+Tab walk the page list, then the page's controls, Up and Down move in the list, Space flips a toggle, Escape closes and
// gives the focus back to what opened it. Scrim, level 3 shadow.
Popup {
    id: panel
    objectName: "settingsPanel"
    required property SettingsModel model

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    property string page: "general" // a page id (pages)
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
    background: Item {
        Shadow { target: box; level: 3; look: panel.look }
        Item { id: box; anchors.fill: parent }
    }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; to: 0; duration: 90; easing.type: Easing.OutCubic }
    }
    // What had the focus before: Escape gives it back (the window's canvas, a menu's button).
    property Item opener: null
    onAboutToShow: {
        error = ""
        model.reload()
        opener = parent ? parent.Window.activeFocusItem : null
    }
    onOpened: {
        const tab = pageTabs.itemAt(Math.max(0, pages.findIndex(p => p.id === page)))
        if (tab)
            tab.forceActiveFocus()
    }
    onClosed: if (opener) opener.forceActiveFocus()

    // Runs a model call and keeps its refusal, if any.
    function apply(result) { error = result; return result === "" }

    // The pages by their stable id, in the order of their translated titles (locale-aware, so each language sorts
    // its own way). Code and tests name a page by id, never by its place.
    readonly property var pages: [{id: "general", title: qsTr("General")}, {id: "folders", title: qsTr("Folders")},
                                  {id: "run", title: qsTr("Run")}, {id: "providers", title: qsTr("Providers")},
                                  {id: "keymap", title: qsTr("Keymap")}, {id: "theme", title: qsTr("Theme")},
                                  {id: "packs", title: qsTr("Packs")}, {id: "cache", title: qsTr("Cache")}]
                                 .sort((a, b) => a.title.localeCompare(b.title))
    // The pages whose changes go to settings.json (the files' note under the page).
    readonly property bool inSettingsFile: page === "general" || page === "folders" || page === "run"

    // The desktop portal's folder picker (Qt's own where there is none).
    property var pickDone: null
    function pickFolder(callback) {
        pickDone = callback
        // A line starting with "(" would continue the line above as a call of `callback`.
        const dialog = folderDialog.createObject(panel) as FilePicker
        dialog.open()
    }
    Component {
        id: folderDialog
        FilePicker {
            id: d
            objectName: "settingsFolderDialog"
            fileMode: PortalFileDialog.Directory
            onAccepted: { if (panel.pickDone) panel.pickDone(d.selectedFolder); d.destroy() }
            onRejected: d.destroy()
        }
    }

    // --- the panel ---------------------------------------------------------------------

    contentItem: KitPanel {
      id: card
      objectName: "settingsCard"
      look: panel.look
      tone: "slate"
      clip: true
      // Tab walks the window's controls: it is also the window's "Add a node by search" key.
      Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab
      ColumnLayout {
        anchors { fill: parent; margins: 1 }
        spacing: 0
        LayoutMirroring.enabled: panel.rightToLeft
        LayoutMirroring.childrenInherit: true

        KitHero {
            id: hero
            objectName: "settingsHero"
            Layout.fillWidth: true
            look: panel.look
            tone: card.tone
            title: qsTr("Settings")
            subtitle: qsTr("How Fugue looks, where it keeps things and how it runs.")
            showClose: true
            onClosed: panel.close()
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // The pages.
            ColumnLayout {
                Layout.preferredWidth: 36 * panel.u
                Layout.fillWidth: false
                Layout.fillHeight: true
                Layout.margins: 4 * panel.u
                spacing: 0
                Repeater {
                    id: pageTabs
                    model: panel.pages
                    delegate: Rectangle {
                        id: tab
                        required property var modelData
                        required property int index
                        readonly property string pageId: modelData.id
                        objectName: "settingsPage-" + pageId
                        Layout.fillWidth: true
                        implicitHeight: 7 * panel.u
                        radius: Math.min(panel.look.params.cornerChip * panel.u, 0.45 * height)
                        // The chosen page takes the window's tone, as a chip that is on does (Templates' categories).
                        color: panel.page === tab.pageId ? Qt.alpha(panel.look.tone(card.tone).glass, 0.34) : tabHover.hovered ? panel.look.kitChipHover : "transparent"
                        border.width: activeFocus && FocusInput.keyboard ? 2 : 0
                        border.color: panel.look.accent
                        activeFocusOnTab: true
                        Accessible.role: Accessible.PageTab
                        Accessible.name: modelData.title
                        Accessible.selected: panel.page === tab.pageId
                        Keys.onSpacePressed: panel.page = tab.pageId
                        Keys.onReturnPressed: panel.page = tab.pageId
                        Keys.onUpPressed: if (index > 0) panel.page = panel.pages[index - 1].id
                        Keys.onDownPressed: if (index < panel.pages.length - 1) panel.page = panel.pages[index + 1].id
                        HoverHandler { id: tabHover }
                        TapHandler { onTapped: { panel.page = tab.pageId; panel.error = "" } }
                        Text {
                            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 2 * panel.u }
                            text: tab.modelData.title
                            color: panel.look.text
                            font.pixelSize: panel.look.textSize
                            font.weight: panel.page === tab.pageId ? Font.DemiBold : Font.Normal
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
                ScrollBar.vertical: FugueScrollBar { look: panel.look }
                ColumnLayout {
                    id: pageColumn
                    x: 6 * panel.u
                    y: 6 * panel.u
                    width: scroller.width - 12 * panel.u - (scroller.ScrollBar.vertical as FugueScrollBar).inset
                    spacing: 3 * panel.u

                    // settings.json cannot be used as it is.
                    Repeater {
                        model: panel.inSettingsFile ? panel.model.problems : []
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
                        visible: panel.page === "general"
                        title: qsTr("Graph navigation") // "canvas" is the picture to a person; the node view is the Graph (#228)
                        key: "navigation"
                        origin: panel.model.origins["navigation"] ?? ""
                        onUseDefaultRequested: panel.apply(panel.model.useDefault("navigation"))
                        note: panel.model.navigation === "legacy"
                              ? qsTr("Left-drag on an empty part of the graph pans and the wheel zooms; Ctrl+drag selects. ComfyUI's default.")
                              : qsTr("Left-drag on an empty part of the graph selects and the wheel pans; Ctrl+wheel zooms.")
                        Choices {
                            Layout.fillWidth: true
                            name: "navigation"
                            current: panel.model.navigation
                            options: [{value: "legacy", text: qsTr("Legacy")}, {value: "standard", text: qsTr("Standard")}]
                            onPicked: value => panel.apply(panel.model.set("navigation", value))
                        }
                    }
                    SettingsParts.Setting {
                        visible: panel.page === "general"
                        title: qsTr("Magnify small nodes on hover")
                        key: "hover_magnifier"
                        origin: panel.model.origins["hover_magnifier"] ?? ""
                        onUseDefaultRequested: panel.apply(panel.model.useDefault("hover_magnifier"))
                        note: qsTr("Zoomed far out, a short hover over a node shows it at full size over itself, so a wire can start there. Off by default.")
                        head: SettingsParts.Toggle {
                            objectName: "hoverMagnifier"
                            text: qsTr("Magnify small nodes on hover")
                            on: panel.model.hoverMagnifier
                            onToggled: value => panel.apply(panel.model.set("hover_magnifier", value))
                        }
                    }
                    SettingsParts.Setting {
                        visible: panel.page === "general"
                        title: qsTr("Node pictures")
                        key: "picture_mode"
                        origin: panel.model.origins["picture_mode"] ?? ""
                        onUseDefaultRequested: panel.apply(panel.model.useDefault("picture_mode"))
                        note: panel.model.pictureMode === "all"
                              ? qsTr("Every node that has a picture shows it.")
                              : panel.model.pictureMode === "sinks"
                                ? qsTr("Only the nodes that load a file and the ones that end a chain (Preview, Save, Compare) show a picture.")
                                : qsTr("Nodes that make a picture, load one or end a chain show it; a node that only changes the one before it does not. A node's right-click menu can show or hide its picture.")
                        Choices {
                            Layout.fillWidth: true
                            name: "pictureMode"
                            current: panel.model.pictureMode
                            options: [{value: "smart", text: qsTr("Smart")}, {value: "sinks", text: qsTr("Sinks and inputs only")},
                                      {value: "all", text: qsTr("All")}]
                            onPicked: value => panel.apply(panel.model.set("picture_mode", value))
                        }
                    }
                    SettingsParts.Setting {
                        visible: panel.page === "general"
                        title: qsTr("Sampler preview")
                        key: "step_preview"
                        origin: panel.model.origins["step_preview"] ?? ""
                        onUseDefaultRequested: panel.apply(panel.model.useDefault("step_preview"))
                        note: panel.model.stepPreview === "off"
                              ? qsTr("The Sampler's card shows nothing until the image is done.")
                              : panel.model.stepPreview === "fast"
                                ? qsTr("The Sampler's card shows a rough picture of the image about eight times while it is made. It costs almost no time. Only for a model on this machine.")
                                : qsTr("The Sampler's card shows the image as the model's decoder draws it, about eight times while it is made. It is clearer and costs a little more time and memory. Only for a model on this machine.")
                        Choices {
                            Layout.fillWidth: true
                            name: "stepPreview"
                            current: panel.model.stepPreview === "tae" ? "vae" : panel.model.stepPreview
                            options: [{value: "off", text: qsTr("Off")}, {value: "fast", text: qsTr("Fast")},
                                      {value: "vae", text: qsTr("Full picture")}]
                            onPicked: value => panel.apply(panel.model.set("step_preview", value))
                        }
                    }
                    SettingsParts.Setting {
                        visible: panel.page === "general"
                        title: qsTr("Reopen tabs at start")
                        key: "restore_tabs"
                        origin: panel.model.origins["restore_tabs"] ?? ""
                        onUseDefaultRequested: panel.apply(panel.model.useDefault("restore_tabs"))
                        note: qsTr("Start with the saved workflows that were open when this session ended, on the tab you were on. Each project and the Default session remember their own. A new workflow that was never saved does not come back (its unsaved changes are offered as a recovery copy after a crash). Starting Fugue with a file opens only that file.")
                        head: SettingsParts.Toggle {
                            objectName: "restoreTabs"
                            text: qsTr("Reopen tabs at start")
                            on: panel.model.restoreTabs
                            onToggled: value => panel.apply(panel.model.set("restore_tabs", value))
                        }
                    }
                    SettingsParts.Setting {
                        visible: panel.page === "general"
                        title: qsTr("Language")
                        key: "language"
                        origin: panel.model.origins["language"] ?? ""
                        onUseDefaultRequested: panel.apply(panel.model.useDefault("language"))
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
                        visible: panel.page === "folders"
                        title: qsTr("Output folder")
                        note: qsTr("Where Save Image writes. A Save node's own folder is inside it.")
                        host: panel
                        key: "output_folder"
                        value: panel.model.outputFolder
                        isDefault: panel.model.outputFolderIsDefault
                    }
                    SettingsParts.FolderSetting {
                        visible: panel.page === "folders"
                        title: qsTr("Inputs folder")
                        note: qsTr("Where images pasted or dropped into Load Image are kept. By default inside the output folder.")
                        host: panel
                        key: "inputs_folder"
                        value: panel.model.inputsFolder
                        isDefault: panel.model.inputsFolderIsDefault
                    }
                    PathsSetting {
                        visible: panel.page === "folders"
                        title: qsTr("Extra model folders")
                        note: qsTr("Searched for models besides Fugue's own: a ComfyUI models folder as it is, or a folder of one kind of model, such as LoRAs.")
                        host: panel
                        key: "model_paths"
                        value: panel.model.modelPaths
                        inherited: panel.model.inheritedModelPaths
                        kinds: true
                    }
                    SettingsParts.Setting {
                        visible: panel.page === "folders"
                        title: qsTr("Download models into")
                        note: qsTr("Where models are saved when Fugue downloads them for a template or a workflow, in the folder of their type. Choose from Fugue's own folder and the extra model folders above.")
                        key: "download_folder"
                        Choices {
                            Layout.fillWidth: true
                            name: "downloadFolder"
                            current: panel.model.downloadFolder
                            options: panel.model.downloadFolders.map(f => ({value: f.path, text: f.title}))
                            onPicked: value => panel.apply(panel.model.set("download_folder", value))
                        }
                    }
                    PathsSetting {
                        visible: panel.page === "folders"
                        title: qsTr("Extra pack folders")
                        note: qsTr("Searched for node packs besides Fugue's own.")
                        host: panel
                        key: "pack_paths"
                        value: panel.model.packPaths
                        inherited: panel.model.inheritedPackPaths
                    }

                    // Run
                    SettingsParts.Setting {
                        visible: panel.page === "run"
                        title: qsTr("Seeds of new nodes")
                        key: "new_seed"
                        origin: panel.model.origins["new_seed"] ?? ""
                        onUseDefaultRequested: panel.apply(panel.model.useDefault("new_seed"))
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
                        visible: panel.page === "run"
                        title: qsTr("Embed the workflow in new Save Image nodes")
                        key: "embed_workflow"
                        origin: panel.model.origins["embed_workflow"] ?? ""
                        onUseDefaultRequested: panel.apply(panel.model.useDefault("embed_workflow"))
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
                        visible: panel.page === "providers"
                        Layout.fillWidth: true
                        host: panel
                    }

                    // Packs
                    PacksPage {
                        visible: panel.page === "packs"
                        Layout.fillWidth: true
                        host: panel
                    }

                    // Cache (#141): made when first shown, so the page costs nothing until then.
                    Loader {
                        active: panel.page === "cache" || item !== null
                        visible: panel.page === "cache"
                        Layout.fillWidth: true
                        sourceComponent: CachePage { host: panel }
                    }

                    // Keymap
                    SettingsParts.Setting {
                        visible: panel.page === "keymap"
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

                    // Theme: the kit's look (a preset and six sliders, live; Reset puts them back) as one group of rows, then what the
                    // Omarchy theme says. The sliders change parameters, never per-frame work (docs/canvas-look.md 20.13).
                    Column {
                        id: themePage
                        visible: panel.page === "theme"
                        Layout.fillWidth: true
                        readonly property real frostAmount: panel.model.frost
                        component LookRow: KitRow {
                            id: lookRow
                            required property string settingKey
                            required property string caption
                            required property real amount
                            property alias objectNameOfSlider: slide.objectName
                            look: panel.look
                            label: caption
                            KitSlider {
                                id: slide
                                look: panel.look
                                railWidth: 36 * panel.u
                                from: 0
                                to: 1
                                stepSize: 0.05
                                value: lookRow.amount
                                showValue: true
                                displayScale: 100
                                suffix: "%"
                                text: lookRow.caption
                                onMoved: v => panel.apply(panel.model.set(lookRow.settingKey, Math.round(v * 20) / 20))
                            }
                        }
                        KitSection { look: panel.look; text: qsTr("Look"); topPadding: 0 }
                        KitGroup {
                            look: panel.look
                            KitRow {
                                look: panel.look
                                label: qsTr("Preset")
                                hint: qsTr("Calm is the full look, Flat the plain one.")
                                KitSegmented {
                                    id: presets
                                    objectName: "lookPreset"
                                    look: panel.look
                                    text: qsTr("Look")
                                    readonly property var shown: panel.model.lookPresets.concat([{value: "custom", text: qsTr("Custom")}])
                                    options: shown.map(o => o.text)
                                    optionNames: shown.map(o => "lookPreset-" + o.value)
                                    currentIndex: Math.max(0, shown.findIndex(o => o.value === panel.model.lookPreset))
                                    onMoved: index => { if (shown[index].value !== "custom") panel.apply(panel.model.applyLookPreset(shown[index].value)) }
                                }
                            }
                            // The node card's face (docs/canvas-look.md 20.14, #466): live; the Flat preset also sets it, and the
                            // two can be mixed.
                            KitRow {
                                look: panel.look
                                label: qsTr("Node style")
                                hint: qsTr("Compact cards, or the flat face.")
                                KitSegmented {
                                    objectName: "nodeStyle"
                                    look: panel.look
                                    text: qsTr("Node style")
                                    options: [qsTr("Compact"), qsTr("Flat")]
                                    currentIndex: panel.model.nodeStyle === "flat" ? 1 : 0
                                    onMoved: index => panel.apply(panel.model.set("node_style", index === 1 ? "flat" : "compact"))
                                }
                            }
                            LookRow { objectNameOfSlider: "lookColour"; settingKey: "look_colour"; caption: qsTr("Colour"); amount: panel.model.lookColour }
                            LookRow { objectNameOfSlider: "lookGlow"; settingKey: "look_glow"; caption: qsTr("Glow"); amount: panel.model.lookGlow }
                            KitRow {
                                id: frostRow
                                look: panel.look
                                label: qsTr("Frost")
                                hint: themePage.frostAmount <= 0 ? qsTr("Off: the buttons over a picture are plain.")
                                    : themePage.frostAmount < 0.4 ? qsTr("Light: faintly frosted.")
                                    : themePage.frostAmount < 0.75 ? qsTr("Medium: frosted glass, the picture blurred behind.")
                                    : qsTr("Strong: thick frosted glass.")
                                SettingsParts.SmallButton {
                                    objectName: "useDefault-frost"
                                    visible: (panel.model.origins["frost"] ?? "") === "override"
                                    text: qsTr("Use Default's value")
                                    onClicked: panel.apply(panel.model.useDefault("frost"))
                                }
                                KitSlider {
                                    objectName: "frost"
                                    look: panel.look
                                    railWidth: 36 * panel.u
                                    from: 0
                                    to: 1
                                    stepSize: 0.1
                                    value: themePage.frostAmount
                                    text: qsTr("Frost")
                                    showValue: true
                                    displayScale: 100
                                    suffix: "%"
                                    onMoved: v => panel.apply(panel.model.set("frost", Math.round(v * 10) / 10))
                                }
                            }
                            LookRow { objectNameOfSlider: "lookDepth"; settingKey: "look_depth"; caption: qsTr("Depth"); amount: panel.model.lookDepth }
                            LookRow { objectNameOfSlider: "lookCorners"; settingKey: "look_corners"; caption: qsTr("Corners"); amount: panel.model.lookCorners }
                            KitRow {
                                look: panel.look
                                label: qsTr("Follow Omarchy's rounding")
                                hint: qsTr("The Corners slider starts from the Omarchy theme's window rounding instead of 8 px.")
                                KitToggle {
                                    objectName: "lookCornersOmarchy"
                                    look: panel.look
                                    text: qsTr("Follow Omarchy's rounding")
                                    checked: panel.model.lookCornersOmarchy
                                    onToggled: value => panel.apply(panel.model.set("look_corners_omarchy", value))
                                }
                            }
                            LookRow { objectNameOfSlider: "lookSpacing"; settingKey: "look_spacing"; caption: qsTr("Spacing"); amount: panel.model.lookSpacing }
                            KitRow {
                                look: panel.look
                                label: qsTr("Reset")
                                hint: qsTr("Back to Calm.")
                                SettingsParts.SmallButton {
                                    objectName: "lookFile"
                                    text: qsTr("Open look file…")
                                    onClicked: panel.model.openLookFile()
                                }
                                SettingsParts.SmallButton {
                                    objectName: "lookReset"
                                    text: qsTr("Reset to defaults")
                                    onClicked: panel.apply(panel.model.applyLookPreset("calm"))
                                }
                            }
                        }
                        Text {
                            width: parent.width
                            topPadding: panel.u
                            leftPadding: 2 * panel.u
                            wrapMode: Text.Wrap
                            color: panel.look.label
                            font.pixelSize: panel.look.labelSize
                            text: qsTr("A preset sets the sliders; moving one makes it Custom. A look.json file in the settings folder can set any look parameter by name.")
                        }
                        KitSection { look: panel.look; text: qsTr("Omarchy theme") }
                        KitGroup {
                            look: panel.look
                            KitRow {
                                look: panel.look
                                label: qsTr("Mode")
                                Text { text: App.theme.light ? qsTr("Light") : qsTr("Dark"); color: panel.look.text; font.pixelSize: panel.look.textSize }
                            }
                            KitRow {
                                look: panel.look
                                label: qsTr("Window rounding")
                                Text { text: App.theme.radius; color: panel.look.text; font.pixelSize: panel.look.textSize }
                            }
                            KitRow {
                                look: panel.look
                                label: qsTr("Text size")
                                Text { text: App.theme.textSize; color: panel.look.text; font.pixelSize: panel.look.textSize }
                            }
                            KitRow {
                                look: panel.look
                                label: qsTr("Colors")
                                hint: qsTr("Fugue follows the Omarchy theme and the desktop text size; change them in Omarchy.")
                                Flow {
                                    id: swatches
                                    width: 52 * panel.u
                                    spacing: panel.u
                                    Repeater {
                                        model: Object.keys(App.theme.colors)
                                        delegate: Rectangle {
                                            required property string modelData
                                            width: 4 * panel.u
                                            height: 4 * panel.u
                                            radius: panel.look.smallCorner
                                            color: App.theme.colors[modelData]
                                            border.width: 1
                                            border.color: panel.look.hairline
                                            Accessible.name: modelData
                                            ToolTip.visible: swatchHover.hovered
                                            ToolTip.delay: App.theme.look.tooltipDelay
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
                text: panel.page === "providers" ? qsTr("Saved to %1 as you change it.").arg(panel.model.providersPath)
                    : panel.inSettingsFile ? qsTr("Saved to %1 as you change it.").arg(panel.model.path) : ""
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
}
