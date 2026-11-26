pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue
import "../kit"

// The Folders page of the settings panel, on the Drift kit and shaped as the Providers page is: one line per place (glyph, name, the
// folder in effect, whether it is the default), one of them open at a time to change it in a tray. The output and inputs folders take a
// path or Choose...; "Download models into" takes one of the model folders; the two lists of extra folders (models, packs) show their
// folders, each with what it holds, and take more. Five rows are no list to search, so there is no search box (Ctrl+F is not bound
// here). The page keeps which row is open by its KEY (the setting's name), so a change that makes the rows again leaves it open.
ColumnLayout {
    id: page
    required property var host // the SettingsPanel: its model, apply() and pickFolder()
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property SettingsModel model: host.model
    spacing: 0

    property string expandedKey // the one open row ("" none)
    property string tabKey: "output_folder" // the last header used: the only one Tab stops on
    property string revealKey // the row that is opening scrolls itself into view while it grows

    readonly property var keys: ["output_folder", "inputs_folder", "model_paths", "download_folder", "pack_paths", "wildcards"]
    readonly property bool downloadIsDefault: model.downloadFolders.length === 0 || model.downloadFolders[0].path === model.downloadFolder // Fugue's own folder comes first
    // The status column's width, shared: as wide as the widest word a row can say (a count up to 99).
    TextMetrics { id: tmDefault; font.pixelSize: page.look.labelSize; text: qsTr("Default") }
    TextMetrics { id: tmCustom; font.pixelSize: page.look.labelSize; text: qsTr("Custom") }
    TextMetrics { id: tmNone; font.pixelSize: page.look.labelSize; text: qsTr("None") }
    TextMetrics { id: tmMany; font.pixelSize: page.look.labelSize; text: qsTr("%n folders", "", 99) }
    readonly property real statusWidth: Math.ceil(Math.max(tmDefault.advanceWidth, tmCustom.advanceWidth, tmNone.advanceWidth, tmMany.advanceWidth))
    function count(n) { return n === 0 ? qsTr("None") : qsTr("%n folders", "", n) }

    function rowOf(key) {
        for (let i = 0; i < rowsHost.children.length; ++i)
            if (rowsHost.children[i].rowKey === key)
                return rowsHost.children[i] as KitDisclosureRow
        return null
    }
    function setExpanded(key, open) {
        if (!open) {
            if (expandedKey === key)
                expandedKey = ""
            return
        }
        if (expandedKey === key)
            return
        const was = rowOf(expandedKey)
        const now = rowOf(key)
        // The row that was open closes at once; when it is above, the page drops by its body so the new row stays under the pointer.
        if (was) {
            was.animate = false
            if (now && was.y < now.y)
                page.host.scrollBy(-was.shown)
        }
        revealKey = key
        revealTimer.restart()
        expandedKey = key
        if (was)
            Qt.callLater(() => was.animate = true)
    }
    Timer { id: revealTimer; interval: 400; onTriggered: page.revealKey = "" }
    // Up, Down, Home and End on a header: the next header, none past the ends.
    function step(key, delta) {
        const at = keys.indexOf(key)
        const to = Math.abs(delta) >= 1000 ? (delta < 0 ? 0 : keys.length - 1) : at + delta
        if (to >= 0 && to < keys.length)
            rowOf(keys[to]).focusHeader()
    }

    // The note that opens a row's body: what the setting does, dim, as the Providers rows' first line.
    component Note: Item {
        id: note
        property string text
        implicitWidth: parent ? parent.width : 0
        implicitHeight: noteText.implicitHeight + 3 * page.u
        Text {
            id: noteText
            x: page.look.params.rowPadLeft * page.u
            y: 1.5 * page.u
            width: parent.width - (page.look.params.rowPadLeft + page.look.params.rowPadRight) * page.u
            text: note.text
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: page.look.label
            font.pixelSize: page.look.labelSize
        }
    }

    // A folder row of the page's list: the shared wiring of a KitDisclosureRow.
    component FolderRow: KitDisclosureRow {
        id: folderRow
        readonly property string key: rowKey
        look: page.look
        glyph: "folder"
        detailWidth: Math.min(48 * page.u, 0.45 * page.width)
        expanded: page.expandedKey === rowKey
        tabStop: page.tabKey === rowKey
        detailElide: Text.ElideMiddle
        statusWidth: page.statusWidth
        statusName: "folderStatus-" + rowKey
        onExpandRequested: value => page.setExpanded(rowKey, value)
        onStepRequested: delta => page.step(rowKey, delta)
        onShownChanged: if (page.revealKey === rowKey) page.host.showAll(folderRow, header.height + shown)
        Connections {
            target: folderRow.header
            function onActiveFocusChanged() { if (folderRow.header.activeFocus) page.tabKey = folderRow.rowKey }
        }
    }

    // The output and inputs folders: the path (Enter commits), Choose..., and Default when it is not.
    component FolderBody: Item {
        id: body
        required property string settingKey
        property string value
        property bool isDefault
        property string note
        implicitWidth: parent ? parent.width : 0
        implicitHeight: column.height
        Column {
            id: column
            width: parent.width
            Note { text: body.note }
            KitRow {
                look: page.look
                label: qsTr("Folder")
                SettingsParts.Field {
                    implicitWidth: Math.min(60 * page.u, page.width - 16 * page.u) // never wider than the tray, narrow or wide
                    objectName: body.settingKey
                    label: qsTr("Folder")
                    value: body.value
                    placeholder: qsTr("Path of the folder")
                    onCommitted: text => page.host.apply(page.model.set(body.settingKey, text))
                }
            }
            KitRow {
                look: page.look
                label: qsTr("Choose a folder")
                hint: qsTr("Opens your file chooser")
                KitChip {
                    objectName: body.settingKey + "-choose"
                    look: page.look
                    text: qsTr("Choose…")
                    onTapped: page.host.pickFolder(url => page.host.apply(page.model.set(body.settingKey, url)))
                }
                KitChip {
                    objectName: body.settingKey + "-default"
                    look: page.look
                    visible: !body.isDefault
                    text: qsTr("Default")
                    onTapped: page.host.apply(page.model.set(body.settingKey, ""))
                }
            }
        }
    }

    Text {
        Layout.fillWidth: true
        Layout.leftMargin: 2 * page.u
        text: qsTr("Where Fugue saves pictures, downloads models, and looks for models, packs and wildcards.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: page.look.label
        font.pixelSize: page.look.labelSize
    }

    KitGroup {
        objectName: "folderList"
        Layout.fillWidth: true
        Layout.topMargin: 3 * page.u
        look: page.look
        Column {
            id: rowsHost
            width: parent.width

            FolderRow {
                objectName: "folderRow-output_folder"
                rowKey: "output_folder"
                bodyName: "folderBody-output_folder"
                label: qsTr("Output folder")
                detail: page.model.outputFolder
                where: page.model.outputFolder
                status: page.model.outputFolderIsDefault ? qsTr("Default") : qsTr("Custom")
                FolderBody {
                    settingKey: "output_folder"
                    note: qsTr("Where Save Image writes. A Save node's own folder is inside it.")
                    value: page.model.outputFolder
                    isDefault: page.model.outputFolderIsDefault
                }
            }
            FolderRow {
                objectName: "folderRow-inputs_folder"
                rowKey: "inputs_folder"
                bodyName: "folderBody-inputs_folder"
                label: qsTr("Inputs folder")
                detail: page.model.inputsFolder
                where: page.model.inputsFolder
                status: page.model.inputsFolderIsDefault ? qsTr("Default") : qsTr("Custom")
                FolderBody {
                    settingKey: "inputs_folder"
                    note: qsTr("Where images pasted or dropped into Load Image are kept. By default inside the output folder.")
                    value: page.model.inputsFolder
                    isDefault: page.model.inputsFolderIsDefault
                }
            }
            FolderRow {
                objectName: "folderRow-model_paths"
                rowKey: "model_paths"
                bodyName: "folderBody-model_paths"
                label: qsTr("Extra model folders")
                status: page.count(page.model.modelPaths.length + page.model.inheritedModelPaths.length)
                PathsList {
                    host: page.host
                    key: "model_paths"
                    note: qsTr("Searched for models besides Fugue's own: a ComfyUI models folder as it is, or a folder of one kind of model, such as LoRAs.")
                    value: page.model.modelPaths
                    inherited: page.model.inheritedModelPaths
                    kinds: true
                }
            }
            FolderRow {
                objectName: "folderRow-download_folder"
                rowKey: "download_folder"
                bodyName: "folderBody-download_folder"
                label: qsTr("Download models into")
                glyph: "download"
                detail: page.model.downloadFolder
                where: page.model.downloadFolder
                status: page.downloadIsDefault ? qsTr("Default") : qsTr("Custom")
                Column {
                    width: parent.width
                    Note { text: qsTr("Where models are saved when Fugue downloads them for a template or a workflow, in the folder of their type. Choose from Fugue's own folder and the extra model folders.") }
                    Item {
                        width: parent.width
                        height: choices.height + 3 * page.u
                        Choices {
                            id: choices
                            x: page.look.params.rowPadLeft * page.u
                            y: 0
                            width: parent.width - (page.look.params.rowPadLeft + page.look.params.rowPadRight) * page.u
                            name: "downloadFolder"
                            current: page.model.downloadFolder
                            options: page.model.downloadFolders.map(f => ({value: f.path, text: f.title}))
                            onPicked: value => page.host.apply(page.model.set("download_folder", value))
                        }
                    }
                }
            }
            FolderRow {
                objectName: "folderRow-pack_paths"
                rowKey: "pack_paths"
                bodyName: "folderBody-pack_paths"
                label: qsTr("Extra pack folders")
                status: page.count(page.model.packPaths.length + page.model.inheritedPackPaths.length)
                PathsList {
                    host: page.host
                    key: "pack_paths"
                    note: qsTr("Searched for node packs besides Fugue's own.")
                    value: page.model.packPaths
                    inherited: page.model.inheritedPackPaths
                }
            }
            FolderRow {
                objectName: "folderRow-wildcards"
                rowKey: "wildcards"
                bodyName: "folderBody-wildcards"
                label: qsTr("Wildcards")
                detail: page.model.wildcardsFolder
                where: page.model.wildcardsFolder
                status: qsTr("Default")
                Note { text: qsTr("Where Dynamic Prompt looks for __name__ files: .txt (one value per line), .yaml and .json, in folders as they come from a collection. A pack's own wildcards folder is searched too, and a project that inherits prompts also reads the Default's. Fugue's own starter lists (places, eras, weather, art-styles and more) come last, so a file of yours with the same name replaces one.") }
            }
        }
    }
}
