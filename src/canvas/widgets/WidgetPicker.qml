pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import ".."

// What a widget opens beyond its field: the menu of a choice, seed or model widget, and the
// file, folder and color pickers (the desktop portal's, Qt's own where there is none). An
// InputWidget makes one the first time it needs it.
Item {
    id: picker
    required property InputWidget host

    // `entries` null: the host's model list, kept current while models arrive. `follow` "canvases":
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
            searchable: entries === null || entries.length > 12,
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

    // New canvas… (#211): a name and a size, or a picture file to make it from. The canvas is made in
    // this root's store and its id put in the input as one undo step, or the reason is shown in the
    // dialog. The Question is made on each use.
    function askNewCanvas() {
        (newCanvasAsk.createObject(picker) as Question).open()
    }
    // Import picture…: the file dialog, then a canvas named after the file. From the New canvas
    // dialog (`inAsk` true) the picked file goes back to it instead.
    property var pictureTarget: null
    function askPicture(inAsk, ask) {
        pictureTarget = inAsk ? ask : null
        (pictureDialog.createObject(picker) as FileDialog).open()
    }
    Component {
        id: pictureDialog
        FileDialog {
            id: d
            objectName: "canvasPictureDialog"
            title: qsTr("Make a canvas from a picture")
            fileMode: FileDialog.OpenFile
            nameFilters: [qsTr("Pictures and canvases (*.png *.jpg *.jpeg *.webp *.ora)"), qsTr("All files (*)")]
            onAccepted: { picker.pictureChosen(d.selectedFile); d.destroy() }
            onRejected: d.destroy()
        }
    }
    // A picture chosen: a canvas at once, or the New canvas dialog takes it.
    function pictureChosen(url) {
        if (pictureTarget) {
            pictureTarget.picture = url
            return
        }
        host.error = host.edits.importCanvas(host.nodeId, host.name, url, "")
    }
    Component {
        id: newCanvasAsk
        Question {
            id: ask
            objectName: "newCanvasAsk"
            // A picture file URL when the canvas is made from one, else empty (a blank canvas).
            property string picture: ""
            property string problem: ""
            readonly property string pictureName: picture === "" ? "" : decodeURIComponent(picture.toString().split("/").pop())
            readonly property string pictureSize: picture === "" ? "" : picker.host.edits.pictureSize(picture)
            parent: Overlay.overlay
            onClosed: ask.destroy()
            onOpened: { nameField.forceActiveFocus(); nameField.selectAll() }
            function create() {
                problem = picture !== ""
                    ? picker.host.edits.importCanvas(picker.host.nodeId, picker.host.name, picture, nameField.text)
                    : picker.host.edits.newCanvas(picker.host.nodeId, picker.host.name, nameField.text,
                                                  parseInt(widthField.text), parseInt(heightField.text))
                if (problem === "")
                    ask.close()
            }
            ColumnLayout {
                anchors.fill: parent
                spacing: 3 * ask.look.unit
                Label { text: qsTr("New canvas"); font.bold: true; Layout.fillWidth: true }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 3 * ask.look.unit
                    rowSpacing: ask.look.unit
                    Label { text: qsTr("Name"); color: ask.look.label }
                    ThemedTextField {
                        id: nameField
                        objectName: "newCanvasName"
                        Layout.fillWidth: true
                        text: ask.pictureName === "" ? qsTr("Untitled") : ask.pictureName.replace(/\.[^.]*$/, "")
                        onAccepted: ask.create()
                    }
                    Label { text: qsTr("Size"); color: ask.look.label; visible: ask.picture === "" }
                    RowLayout {
                        visible: ask.picture === ""
                        Layout.fillWidth: true
                        ThemedTextField {
                            id: widthField
                            objectName: "newCanvasWidth"
                            Layout.preferredWidth: 90
                            text: "1024"
                            inputMethodHints: Qt.ImhDigitsOnly
                            onAccepted: ask.create()
                        }
                        Label { text: "×"; color: ask.look.label }
                        ThemedTextField {
                            id: heightField
                            objectName: "newCanvasHeight"
                            Layout.preferredWidth: 90
                            text: "1024"
                            inputMethodHints: Qt.ImhDigitsOnly
                            onAccepted: ask.create()
                        }
                        Label { text: qsTr("px"); color: ask.look.label }
                    }
                    Label { text: qsTr("Picture"); color: ask.look.label; visible: ask.picture !== "" }
                    Label {
                        objectName: "newCanvasPictureLine"
                        visible: ask.picture !== ""
                        Layout.fillWidth: true
                        text: ask.pictureSize === "" ? ask.pictureName : qsTr("%1 · %2").arg(ask.pictureName).arg(ask.pictureSize)
                        elide: Text.ElideMiddle
                    }
                }
                ThemedButton {
                    objectName: "newCanvasFromPicture"
                    flat: true
                    text: ask.picture === "" ? qsTr("From a picture…") : qsTr("Blank canvas instead")
                    onClicked: ask.picture === "" ? picker.askPicture(true, ask) : ask.picture = ""
                }
                Label {
                    objectName: "newCanvasProblem"
                    Layout.fillWidth: true
                    visible: ask.problem !== ""
                    text: ask.problem
                    color: ask.look.error
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: 2 * ask.look.unit
                    ThemedButton { objectName: "newCanvasCancel"; text: qsTr("Cancel"); onClicked: ask.close() }
                    ThemedButton {
                        objectName: "newCanvasOk"
                        text: qsTr("Create")
                        highlighted: true
                        onClicked: ask.create()
                    }
                }
            }
        }
    }

    function openDialog() {
        const kind = host.kind
        if (kind === "color")
            (colorDialog.createObject(picker) as ColorDialog).open()
        else if (kind === "folder")
            (folderDialog.createObject(picker) as FolderDialog).open()
        else
            (fileDialog.createObject(picker) as FileDialog).open()
    }
    Component {
        id: fileDialog
        FileDialog {
            id: d
            objectName: "widgetFileDialog"
            title: picker.host.port.label
            fileMode: FileDialog.OpenFile
            onAccepted: { picker.host.edits.setPath(picker.host.nodeId, picker.host.name, d.selectedFile); d.destroy() }
            onRejected: d.destroy()
        }
    }
    Component {
        id: folderDialog
        FolderDialog {
            id: d
            objectName: "widgetFolderDialog"
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
