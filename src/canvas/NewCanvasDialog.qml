pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Fugue

// New canvas (#232, epic #206, spec 14.5): Photoshop's New Document, on a Question. A name; where the canvas
// starts (blank, the picture on the clipboard, a picture file); for a blank one a preset or a typed size and a
// background; a preview and one line with the size in megapixels and the cap; Cancel, then the one highlighted
// Create at the right. The work is NewCanvasModel's. `openNew()` is what every entry point calls (the command
// canvas.new, a canvas input's menu); `created(id)` says which canvas was made, and each host decides what shows it.
Question {
    id: root
    objectName: "newCanvasDialog"
    // The canvas store's folder; empty is the session's.
    property string storeRoot: ""
    readonly property alias model: backend
    signal created(string id)
    width: Math.min((parent ? parent.width : 560) - 12 * look.unit, 560)

    // Opens on the last settings, the clipboard's picture when there is one; `why` is a reason to show at once
    // (a paste that could not be made).
    function openNew(why) {
        backend.reset()
        if (why)
            backend.showError(why)
        open()
    }
    onOpened: nameField.forceActiveFocus(Qt.OtherFocusReason)

    NewCanvasModel {
        id: backend
        storeRoot: root.storeRoot
        onCreated: id => {
            root.close()
            root.created(id)
        }
    }

    // One row of choices: the chosen one in the quiet marking, the accent stays for Create.
    component Choice: ViewButton {
        look: root.look
        accented: false
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 3 * root.look.unit
        Label { text: qsTr("New canvas"); font.bold: true; Layout.fillWidth: true }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 3 * root.look.unit
            rowSpacing: 2 * root.look.unit
            Label { text: qsTr("Name"); color: root.look.label }
            ThemedTextField {
                id: nameField
                objectName: "newCanvasName"
                Layout.fillWidth: true
                text: backend.name
                onTextEdited: backend.name = text
                onAccepted: root.create()
            }

            Label { text: qsTr("Start from"); color: root.look.label }
            Flow {
                Layout.fillWidth: true
                spacing: root.look.unit
                Choice {
                    objectName: "newCanvasSourceBlank"
                    text: qsTr("Blank")
                    current: backend.source === "blank"
                    onTapped: backend.source = "blank"
                }
                Choice {
                    objectName: "newCanvasSourceClipboard"
                    text: qsTr("Clipboard")
                    enabled: backend.clipboardAvailable
                    tip: backend.clipboardAvailable ? qsTr("The picture on the clipboard: %1 px").arg(backend.clipboardSize)
                                                    : qsTr("Copy a picture first")
                    current: backend.source === "clipboard"
                    onTapped: backend.source = "clipboard"
                }
                Choice {
                    objectName: "newCanvasSourceFile"
                    text: qsTr("A file…")
                    current: backend.source === "file"
                    onTapped: fileDialog.open()
                }
            }

            Label { text: qsTr("Preset"); color: root.look.label; visible: backend.source === "blank" }
            Flow {
                visible: backend.source === "blank"
                Layout.fillWidth: true
                spacing: root.look.unit
                Repeater {
                    model: backend.presets()
                    Choice {
                        required property var modelData
                        objectName: "newCanvasPreset-" + modelData.id
                        text: modelData.label
                        tip: modelData.tip
                        current: backend.preset === modelData.id
                        onTapped: {
                            backend.choosePreset(modelData.id)
                            if (modelData.id === "custom")
                                widthField.forceActiveFocus()
                        }
                    }
                }
            }

            Label { text: qsTr("Size"); color: root.look.label }
            RowLayout {
                Layout.fillWidth: true
                spacing: root.look.unit
                ThemedTextField {
                    id: widthField
                    objectName: "newCanvasWidth"
                    Layout.preferredWidth: 22 * root.look.unit
                    enabled: backend.source === "blank"
                    text: backend.widthText
                    inputMethodHints: Qt.ImhDigitsOnly
                    onTextEdited: backend.widthText = text
                    onAccepted: root.create()
                    Accessible.name: qsTr("Width in pixels")
                }
                Label { text: "×"; color: root.look.label }
                ThemedTextField {
                    id: heightField
                    objectName: "newCanvasHeight"
                    Layout.preferredWidth: 22 * root.look.unit
                    enabled: backend.source === "blank"
                    text: backend.heightText
                    inputMethodHints: Qt.ImhDigitsOnly
                    onTextEdited: backend.heightText = text
                    onAccepted: root.create()
                    Accessible.name: qsTr("Height in pixels")
                }
                Label { text: qsTr("px"); color: root.look.label }
                Label {
                    objectName: "newCanvasSourceLine"
                    Layout.fillWidth: true
                    Layout.leftMargin: 2 * root.look.unit
                    visible: backend.source !== "blank"
                    text: backend.source === "file" ? backend.fileLine : qsTr("the picture on the clipboard")
                    color: root.look.label
                    elide: Text.ElideMiddle
                }
            }

            Label { text: qsTr("Background"); color: root.look.label; visible: backend.source === "blank" }
            Flow {
                visible: backend.source === "blank"
                Layout.fillWidth: true
                spacing: root.look.unit
                Repeater {
                    model: [{id: "transparent", text: qsTr("Transparent")}, {id: "white", text: qsTr("White")},
                            {id: "black", text: qsTr("Black")}, {id: "colour", text: qsTr("Color")}]
                    Choice {
                        required property var modelData
                        objectName: "newCanvasBackground-" + modelData.id
                        text: modelData.text
                        current: backend.background === modelData.id
                        onTapped: backend.background = modelData.id
                    }
                }
                ThemedTextField {
                    id: colourField
                    objectName: "newCanvasColour"
                    visible: backend.background === "colour"
                    width: 22 * root.look.unit
                    height: 7 * root.look.unit
                    text: backend.colour
                    onTextEdited: backend.colour = text
                    onAccepted: root.create()
                    Accessible.name: qsTr("Background color")
                }
                Choice {
                    objectName: "newCanvasColourPick"
                    visible: backend.background === "colour"
                    glyph: "palette"
                    tip: qsTr("Pick the color")
                    onTapped: colourPopup.openWith(backend.colour)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 3 * root.look.unit
            // The shape of the canvas, in its background (a checkerboard for transparent).
            Item {
                id: preview
                objectName: "newCanvasPreview"
                readonly property real box: 14 * root.look.unit
                readonly property real ratio: backend.width > 0 && backend.height > 0 ? backend.width / backend.height : 1
                readonly property real shapeWidth: ratio >= 1 ? box : box * ratio
                readonly property real shapeHeight: ratio >= 1 ? box / ratio : box
                readonly property bool transparent: backend.source !== "blank" || backend.background === "transparent"
                Layout.preferredWidth: box
                Layout.preferredHeight: box
                Rectangle {
                    anchors.centerIn: parent
                    width: Math.max(preview.shapeWidth, 2)
                    height: Math.max(preview.shapeHeight, 2)
                    border.color: root.look.hairlineStrong
                    color: backend.source !== "blank" || backend.background === "transparent" ? root.look.well
                         : backend.background === "white" ? "#ffffff"
                         : backend.background === "black" ? "#000000" : backend.colour
                    clip: true
                    Grid {
                        visible: preview.transparent
                        anchors.fill: parent
                        columns: Math.ceil(parent.width / (2 * root.look.unit))
                        Repeater {
                            model: preview.transparent ? Math.ceil(parent.width / (2 * root.look.unit)) * Math.ceil(parent.height / (2 * root.look.unit)) : 0
                            Rectangle {
                                required property int index
                                width: 2 * root.look.unit
                                height: width
                                color: (index % Math.ceil(parent.width / (2 * root.look.unit)) + Math.floor(index / Math.ceil(parent.width / (2 * root.look.unit)))) % 2 === 0
                                       ? root.look.hairline : root.look.well
                            }
                        }
                    }
                }
            }
            Label {
                objectName: "newCanvasSummary"
                Layout.fillWidth: true
                text: backend.summary
                color: root.look.label
                font.pixelSize: root.look.captionSize
                wrapMode: Text.Wrap
            }
        }

        Label {
            objectName: "newCanvasProblem"
            Layout.fillWidth: true
            visible: text !== ""
            text: backend.error !== "" ? backend.error : backend.problem
            color: root.look.error
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 2 * root.look.unit
            ThemedButton { objectName: "newCanvasCancel"; text: qsTr("Cancel"); onClicked: root.close() }
            ThemedButton {
                objectName: "newCanvasOk"
                text: qsTr("Create")
                highlighted: true
                enabled: backend.canCreate
                onClicked: root.create()
            }
        }
    }

    function create() {
        if (backend.canCreate)
            backend.create()
    }

    FileDialog {
        id: fileDialog
        objectName: "newCanvasFileDialog"
        title: qsTr("Make a canvas from a picture") // the same file types as Import picture…
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Pictures and canvases (*.png *.jpg *.jpeg *.webp *.ora)"), qsTr("All files (*)")]
        onAccepted: backend.file = fileDialog.selectedFile.toString()
    }
    ColorPickerPopup {
        id: colourPopup
        onPicked: color => backend.colour = color
    }
}
