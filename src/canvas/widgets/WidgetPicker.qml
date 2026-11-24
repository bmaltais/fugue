pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Fugue
import ".."
import "../../menu"

// What a widget opens beyond its field: the menu of a choice, seed or model widget, and the
// file, folder and color pickers (the desktop portal's, through FilePicker; Qt's own where there is none). An
// InputWidget makes one the first time it needs it.
Item {
    id: picker
    required property InputWidget host

    // `entries` null: the host's model list, kept current while models arrive. `follow` "canvases" or "family" (the family menu, searched by word start):
    // the canvas list, rebuilt when the root's canvases change.
    function openMenu(entries, follow) {
        const well = host
        const at = well.mapToItem(Overlay.overlay, 0, well.height)
        const end = well.mapToItem(Overlay.overlay, well.width, 0)
        const menu = menuComponent.createObject(picker, {
            wantX: at.x, wantY: at.y, width: Math.max(end.x - at.x, 120), anchorHeight: well.height,
            entries: entries !== null ? entries : host.modelEntries(), followModels: entries === null,
            followCanvases: follow === "canvases",
            // A model list, or a long one, is searched (#84).
            searchable: entries === null || entries.length > 12 || follow === "family",
            wordStart: follow === "family",
            nothingMatches: entries === null ? qsTr("No model matches “%1”") : qsTr("Nothing matches “%1”")
        }) as WidgetMenu
        menu.open()
    }
    Component {
        id: menuComponent
        WidgetMenu {
            id: m
            property bool followModels: false
            property bool followCanvases: false
            look: picker.host.look
            parent: Overlay.overlay
            onPicked: value => picker.host.picked(value)
            onClosed: m.destroy()
            Connections {
                target: picker.host.edits
                enabled: m.followModels
                function onModelsChanged() { m.entries = picker.host.modelEntries() }
            }
            Connections {
                target: picker.host.edits
                enabled: m.followCanvases
                function onCanvasesChanged() { m.entries = picker.host.canvasEntries() }
            }
        }
    }

    // A catalog model picked (#134): what it is, its license, its size, where it comes from and
    // where it will go, and nothing is fetched until Download. The Question is made on each use.
    function confirmDownload(id) {
        const info = host.edits.catalogInfo(host.nodeId, host.name, id)
        if (info.id === undefined)
            return
        const ask = downloadAsk.createObject(picker, { info: info }) as Question
        ask.open()
    }
    Component {
        id: downloadAsk
        Question {
            id: ask
            objectName: "downloadAsk"
            required property var info
            parent: Overlay.overlay
            onClosed: ask.destroy()
            ColumnLayout {
                anchors.fill: parent
                spacing: 3 * ask.look.unit
                Label {
                    Layout.fillWidth: true
                    objectName: "downloadAskTitle"
                    text: qsTr("Download %1?").arg(ask.info.title)
                    font.bold: true
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    visible: ask.info.description !== ""
                    text: ask.info.description
                    color: ask.look.label
                    wrapMode: Text.Wrap
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 3 * ask.look.unit
                    rowSpacing: ask.look.unit
                    Label { text: qsTr("License"); color: ask.look.label }
                    Label {
                        objectName: "downloadAskLicense"
                        Layout.fillWidth: true
                        text: ask.info.licenseLine
                        // Not for commercial use is the one thing to notice: the error color, and the words stay.
                        color: ask.info.commercial ? ask.look.text : ask.look.error
                        wrapMode: Text.Wrap
                    }
                    Label { text: qsTr("Made by"); color: ask.look.label; visible: ask.info.author !== "" }
                    Label { text: ask.info.author; Layout.fillWidth: true; wrapMode: Text.Wrap; visible: ask.info.author !== "" }
                    Label { text: qsTr("Size"); color: ask.look.label }
                    Label { objectName: "downloadAskSize"; text: ask.info.sizeText }
                    Label { text: qsTr("From"); color: ask.look.label }
                    Label { objectName: "downloadAskHost"; text: ask.info.host; Layout.fillWidth: true; elide: Text.ElideMiddle }
                    Label { text: qsTr("Saved in"); color: ask.look.label }
                    Label { objectName: "downloadAskFolder"; text: ask.info.folder; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("The file is kept only if its SHA-256 is the one Fugue lists for it.")
                    color: ask.look.label
                    font.pixelSize: ask.look.captionSize
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: 2 * ask.look.unit
                    ThemedButton {
                        objectName: "downloadAskLicenseLink"
                        flat: true
                        visible: ask.info.licenseUrl !== ""
                        text: qsTr("Read the license")
                        onClicked: Qt.openUrlExternally(ask.info.licenseUrl)
                    }
                    Item { Layout.fillWidth: true }
                    ThemedButton { objectName: "downloadAskCancel"; text: qsTr("Cancel"); onClicked: ask.close() }
                    ThemedButton {
                        objectName: "downloadAskOk"
                        text: qsTr("Download")
                        highlighted: true
                        onClicked: {
                            const why = picker.host.edits.downloadCatalogModel(picker.host.nodeId, picker.host.name, ask.info.id)
                            ask.close()
                            picker.host.error = why
                        }
                    }
                }
            }
        }
    }

    // New canvas… (#211, #232): the New canvas dialog (NewCanvasDialog.qml), made the first time it is asked for. The canvas
    // is made in this root's store and its id put in the input as one undo step, or the reason is shown in the dialog.
    property var newCanvasDialog: null
    function askNewCanvas() {
        if (!newCanvasDialog)
            newCanvasDialog = newCanvasComponent.createObject(picker, { storeRoot: host.edits.canvasStoreRoot() })
        newCanvasDialog.openNew("")
    }
    Component {
        id: newCanvasComponent
        NewCanvasDialog {
            parent: Overlay.overlay
            onCreated: id => picker.host.error = picker.host.edits.useCanvas(picker.host.nodeId, picker.host.name, id)
        }
    }
    // Import picture…: the file dialog, then a canvas named after the file, at once.
    function askPicture() {
        (pictureDialog.createObject(picker) as FilePicker).open()
    }
    Component {
        id: pictureDialog
        FilePicker {
            id: d
            objectName: "canvasPictureDialog"
            title: qsTr("Import a picture as a canvas") // the menu's Import picture…, the CLI's fugue canvas import
            fileMode: PortalFileDialog.OpenFile
            nameFilters: [qsTr("Pictures and canvases (*.png *.jpg *.jpeg *.webp *.ora *.psd)"), qsTr("All files (*)")]
            onAccepted: { picker.host.error = picker.host.edits.importCanvas(picker.host.nodeId, picker.host.name, d.selectedFile, ""); d.destroy() }
            onRejected: d.destroy()
        }
    }

    function openDialog() {
        const kind = host.kind
        if (kind === "color")
            (colorDialog.createObject(picker) as ColorDialog).open()
        else if (kind === "folder")
            (folderDialog.createObject(picker) as FilePicker).open()
        else
            (fileDialog.createObject(picker) as FilePicker).open()
    }
    Component {
        id: fileDialog
        FilePicker {
            id: d
            objectName: "widgetFileDialog"
            title: picker.host.port.label
            fileMode: PortalFileDialog.OpenFile
            onAccepted: { picker.host.edits.setPath(picker.host.nodeId, picker.host.name, d.selectedFile); d.destroy() }
            onRejected: d.destroy()
        }
    }
    Component {
        id: folderDialog
        FilePicker {
            id: d
            objectName: "widgetFolderDialog"
            fileMode: PortalFileDialog.Directory
            title: picker.host.port.label
            onAccepted: { picker.host.edits.setPath(picker.host.nodeId, picker.host.name, d.selectedFolder); d.destroy() }
            onRejected: d.destroy()
        }
    }
    Component {
        id: colorDialog
        ColorDialog {
            id: d
            objectName: "widgetColorDialog"
            title: picker.host.port.label
            options: ColorDialog.ShowAlphaChannel
            selectedColor: picker.host.colorOf(picker.host.valueText, picker.host.look.text)
            onAccepted: { picker.host.edits.setColor(picker.host.nodeId, picker.host.name, d.selectedColor); d.destroy() }
            onRejected: d.destroy()
        }
    }
}
