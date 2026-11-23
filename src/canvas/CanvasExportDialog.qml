pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../menu"

// Export with quality and scale… (#247) and Export layers as files… (#248), the Canvas view's two export dialogs, on a
// Question (epic #206, spec 14.5): Photoshop's Export As / Save for Web and Layers to Files. One dialog with two modes,
// as both are the same questions of one revision. Picture: a format (PNG, JPEG, WebP, or the layers kept as .ora), its
// quality, a size as a percentage or in pixels, a background for the transparency, and a preview of the picture as
// encoded with its size in bytes. Layers: PNG or WebP, hidden layers or not, layer masks applied or not, a name prefix,
// and the list of files with their sizes. Cancel, then the one highlighted Export…, which asks the desktop where
// (FilePicker: a file, or a folder for the layers) and writes; a file is never replaced. The work is CanvasExportModel's,
// the code `fugue canvas export` runs. `openFor()` is what the menu rows call; `saved(notice)` says what was written.
Question {
    id: root
    objectName: "canvasExportDialog"
    // The canvas store's folder; empty is the session's.
    property string storeRoot: ""
    readonly property alias model: backend
    readonly property bool layers: backend.mode === "layers"
    signal saved(string notice)
    width: Math.min((parent ? parent.width : 640) - 12 * look.unit, 640)

    // Opens on a revision of a canvas; `mode` is "picture" or "layers". Returns why not (the dialog stays closed), or "".
    function openFor(canvasId, revision, mode) {
        const why = backend.open(canvasId, revision, mode)
        if (why === "")
            open()
        return why
    }
    onOpened: exportButton.forceActiveFocus(Qt.OtherFocusReason)

    CanvasExportModel {
        id: backend
        storeRoot: root.storeRoot
    }

    // One row of choices: the chosen one in the quiet marking, the accent stays for Export.
    component Choice: ViewButton {
        look: root.look
        accented: false
        checkable: true
    }
    component Caption: Label {
        color: root.look.label
        Layout.alignment: Qt.AlignTop
        Layout.topMargin: 1.5 * root.look.unit
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 3 * root.look.unit
        Label {
            objectName: "canvasExportTitle"
            text: root.layers ? qsTr("Export layers as files") : qsTr("Export with quality and scale")
            font.bold: true
            Layout.fillWidth: true
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 3 * root.look.unit
            // The picture as it will be written: a checkerboard behind it shows its transparency.
            Rectangle {
                id: previewBox
                objectName: "canvasExportPreview"
                visible: !root.layers
                Layout.preferredWidth: 24 * root.look.unit
                Layout.preferredHeight: 24 * root.look.unit
                Layout.alignment: Qt.AlignTop
                color: root.look.well
                border.color: root.look.hairlineStrong
                clip: true
                Image {
                    id: previewImage
                    anchors.fill: parent
                    anchors.margins: 1
                    source: backend.preview
                    asynchronous: true
                    cache: false
                    sourceSize.width: 640
                    sourceSize.height: 640
                    Accessible.role: Accessible.Graphic
                    Accessible.name: qsTr("Preview of the picture to write")
                    fillMode: Image.PreserveAspectFit
                }
                BusyIndicator {
                    anchors.centerIn: parent
                    running: backend.busy
                    visible: running
                    width: 6 * root.look.unit
                    height: width
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 3 * root.look.unit
                rowSpacing: 2 * root.look.unit

                Caption { text: qsTr("Format") }
                Flow {
                    Layout.fillWidth: true
                    spacing: root.look.unit
                    Repeater {
                        model: root.layers ? [{id: "png", text: "PNG"}, {id: "webp", text: "WebP"}]
                                           : [{id: "png", text: "PNG"}, {id: "jpg", text: "JPEG"}, {id: "webp", text: "WebP"},
                                              {id: "ora", text: qsTr("Layers (.ora)")}, {id: "psd", text: qsTr("Photoshop (.psd)")}]
                        Choice {
                            required property var modelData
                            objectName: "canvasExportFormat-" + modelData.id
                            text: modelData.text
                            current: backend.format === modelData.id
                            tip: modelData.id === "ora" ? qsTr("Keep the layers: the OpenRaster file Krita and GIMP open")
                                 : modelData.id === "psd" ? qsTr("Keep the layers: the Photoshop file Photoshop, Affinity, Photopea, Krita and GIMP open") : ""
                            onTapped: backend.format = modelData.id
                        }
                    }
                }

                Caption { text: qsTr("Quality"); visible: backend.canBeLossless || backend.hasQuality }
                RowLayout {
                    Layout.fillWidth: true
                    visible: backend.canBeLossless || backend.hasQuality
                    spacing: root.look.unit
                    Choice {
                        objectName: "canvasExportLossless"
                        visible: backend.canBeLossless
                        text: qsTr("Lossless")
                        tip: qsTr("Every pixel and the transparency kept: larger files")
                        current: backend.lossless
                        onTapped: backend.lossless = !backend.lossless
                    }
                    ValueSlider {
                        objectName: "canvasExportQuality"
                        visible: backend.hasQuality
                        look: root.look
                        label: qsTr("Quality")
                        from: 1
                        to: 100
                        step: 1
                        value: backend.quality
                        shown: String(Math.round(value))
                        tip: qsTr("Lower is smaller and shows more artifacts: watch the preview")
                        onMoved: v => backend.quality = Math.round(v)
                    }
                }

                Caption { text: qsTr("Size"); visible: !root.layers }
                RowLayout {
                    Layout.fillWidth: true
                    visible: !root.layers
                    spacing: root.look.unit
                    Repeater {
                        model: [25, 50, 100, 200]
                        Choice {
                            required property int modelData
                            objectName: "canvasExportScale-" + modelData
                            text: qsTr("%1%").arg(modelData)
                            current: backend.percent === modelData
                            onTapped: backend.percent = modelData
                        }
                    }
                    ThemedTextField {
                        objectName: "canvasExportWidth"
                        Layout.preferredWidth: 20 * root.look.unit
                        Layout.leftMargin: 2 * root.look.unit
                        text: backend.widthText
                        inputMethodHints: Qt.ImhDigitsOnly
                        onTextEdited: backend.widthText = text
                        Accessible.name: qsTr("Width in pixels")
                    }
                    Label { text: "×"; color: root.look.label }
                    ThemedTextField {
                        objectName: "canvasExportHeight"
                        Layout.preferredWidth: 20 * root.look.unit
                        text: backend.heightText
                        inputMethodHints: Qt.ImhDigitsOnly
                        onTextEdited: backend.heightText = text
                        Accessible.name: qsTr("Height in pixels")
                    }
                    Label { text: qsTr("px"); color: root.look.label }
                }

                Caption { text: qsTr("Background"); visible: backend.hasBackground }
                Flow {
                    Layout.fillWidth: true
                    visible: backend.hasBackground
                    spacing: root.look.unit
                    Repeater {
                        model: [{id: "auto", text: qsTr("Automatic")}, {id: "transparent", text: qsTr("Transparent")},
                                {id: "white", text: qsTr("White")}, {id: "black", text: qsTr("Black")}, {id: "colour", text: qsTr("Color")}]
                        Choice {
                            required property var modelData
                            objectName: "canvasExportBackground-" + modelData.id
                            text: modelData.text
                            current: backend.background === modelData.id
                            enabled: !(modelData.id === "transparent" && backend.format === "jpg")
                            tip: modelData.id === "auto" ? qsTr("Transparent, white for a JPEG, which has no transparency")
                               : modelData.id === "transparent" && backend.format === "jpg" ? qsTr("JPEG has no transparency") : ""
                            onTapped: backend.background = modelData.id
                        }
                    }
                    ThemedTextField {
                        objectName: "canvasExportColour"
                        visible: backend.background === "colour"
                        width: 22 * root.look.unit
                        height: 7 * root.look.unit
                        text: backend.colour
                        onTextEdited: backend.colour = text
                        Accessible.name: qsTr("Background color")
                    }
                }

                Caption { text: qsTr("Layers"); visible: root.layers }
                Flow {
                    Layout.fillWidth: true
                    visible: root.layers
                    spacing: root.look.unit
                    Choice {
                        objectName: "canvasExportVisibleOnly"
                        text: qsTr("Visible layers only")
                        tip: qsTr("Leave out the layers whose eye is closed")
                        current: backend.visibleOnly
                        onTapped: backend.visibleOnly = !backend.visibleOnly
                    }
                    Choice {
                        objectName: "canvasExportApplyMasks"
                        text: qsTr("Apply layer masks")
                        tip: qsTr("Cut each layer by its own layer mask")
                        current: backend.applyMasks
                        onTapped: backend.applyMasks = !backend.applyMasks
                    }
                }

                Caption { text: qsTr("Name starts with"); visible: root.layers }
                ThemedTextField {
                    objectName: "canvasExportPrefix"
                    visible: root.layers
                    Layout.fillWidth: true
                    text: backend.prefix
                    onTextEdited: backend.prefix = text
                    Accessible.name: qsTr("File name prefix")
                }
            }
        }

        // The files that would be written, with their sizes (layers).
        ListView {
            id: fileList
            objectName: "canvasExportFiles"
            visible: root.layers
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 24 * root.look.unit)
            clip: true
            model: backend.files
            delegate: RowLayout {
                id: fileRow
                required property var modelData
                width: ListView.view.width
                spacing: 2 * root.look.unit
                Label { text: fileRow.modelData.fileName; elide: Text.ElideMiddle; Layout.fillWidth: true }
                Label { text: fileRow.modelData.size; color: root.look.label }
            }
        }
        Label {
            objectName: "canvasExportSkipped"
            Layout.fillWidth: true
            visible: backend.skipped !== ""
            text: root.layers ? qsTr("Left out, a selection is not a picture: %1").arg(backend.skipped) : backend.skipped
            color: root.look.label
            font.pixelSize: root.look.captionSize
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "canvasExportEstimate"
            Layout.fillWidth: true
            text: backend.problem !== "" ? backend.problem : backend.busy ? qsTr("Working…") : backend.estimate
            color: backend.problem !== "" ? root.look.error : root.look.label
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "canvasExportError"
            Layout.fillWidth: true
            visible: text !== ""
            text: backend.error
            color: root.look.error
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 2 * root.look.unit
            ThemedButton { objectName: "canvasExportCancel"; text: qsTr("Cancel"); onClicked: root.close() }
            ThemedButton {
                id: exportButton
                objectName: "canvasExportOk"
                text: qsTr("Export…")
                highlighted: true
                enabled: backend.canExport
                onClicked: root.chooseTarget()
            }
        }
    }

    // Where: a file for a picture (named for the canvas, with the format's extension), a folder for the layers.
    function chooseTarget() {
        const pictures = StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        target.currentFolder = pictures
        target.fileMode = root.layers ? PortalFileDialog.Directory : PortalFileDialog.SaveFile
        target.title = root.layers ? qsTr("Choose the folder for the layers") : qsTr("Save the picture")
        target.defaultSuffix = backend.format
        target.suggestedName = backend.fileName
        target.selectedFile = root.layers ? "" : pictures + "/" + backend.fileName
        target.nameFilters = root.layers ? [] : [backend.format === "ora" ? qsTr("Layered image (*.ora)")
                                               : backend.format === "psd" ? qsTr("Photoshop file (*.psd)")
                                               : qsTr("%1 images (*.%2)").arg(backend.format.toUpperCase()).arg(backend.format)]
        target.open()
    }
    FilePicker {
        id: target
        objectName: "canvasExportTarget"
        onAccepted: {
            const place = root.layers ? selectedFolder.toString() : selectedFile.toString()
            if (backend.exportTo(place)) {
                const notice = backend.notice
                root.close()
                root.saved(notice)
            }
        }
    }
    Component.onDestruction: target.close()
}
