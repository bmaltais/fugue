pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../kit"
import "../menu"

// Batch… (#250), Canvas > Automate: Photoshop's Automate > Batch on a Question (epic #206). Pick an action, the folder of
// pictures and the folder for the results, and a format; Run makes each picture a canvas, runs the action on it and writes
// the result next to nothing it could replace (a name is taken: -2, -3). It runs in the background through the window's
// runs, one picture at a time: a failed picture is listed with its reason and the batch goes on; Cancel stops after
// ending the run under way. Closing the dialog leaves a batch running; reopening shows where it is. The work is
// CanvasBatch's (App.canvasBatch), the code `fugue canvas batch` runs.
Question {
    id: root
    objectName: "canvasBatchDialog"
    heading: qsTr("Batch")
    subheading: qsTr("One action on every picture of a folder.")
    readonly property var batch: App.canvasBatch
    property string action: ""
    property string inFolder: ""
    property string outFolder: ""
    property string format: "png"
    property string problem: ""
    readonly property var actionRow: batch ? batch.actions().find(row => row.id === action) ?? null : null
    readonly property bool showsResults: batch !== null && (batch.busy || batch.finished)
    readonly property int pictures: batch && inFolder !== "" ? batch.pictureCount(inFolder) : -1
    readonly property bool ready: batch !== null && !batch.busy && actionRow !== null && actionRow.missing === 0 && pictures > 0 && outFolder !== ""
                                  && (!actionRow.needsPrompt || promptField.text.trim() !== "")
    width: Math.min((Overlay.overlay ? Overlay.overlay.width : 640) - 12 * look.unit, 560)

    function openDialog() {
        if (batch && !batch.busy && !batch.finished) {
            const rows = batch.actions()
            if (rows.length > 0 && !rows.some(row => row.id === action))
                action = rows[0].id
        }
        problem = ""
        open()
    }
    onOpened: (root.showsResults ? closeButton : runButton).forceActiveFocus(Qt.OtherFocusReason)

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

    function run() {
        const reply = batch.start({action: action, inFolder: inFolder, outFolder: outFolder, format: format,
                                   quality: 92, prompt: promptField.text})
        problem = reply.ok ? "" : reply.reason
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 3 * root.look.unit
        GridLayout {
            visible: !root.showsResults
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 3 * root.look.unit
            rowSpacing: 2 * root.look.unit

            Caption { text: qsTr("Action") }
            Flow {
                Layout.fillWidth: true
                spacing: root.look.unit
                Repeater {
                    model: root.batch ? root.batch.actions() : []
                    Choice {
                        required property var modelData
                        objectName: "canvasBatchAction-" + modelData.id
                        text: modelData.name
                        enabled: modelData.missing === 0
                        tip: modelData.missing > 0 ? qsTr("Its models are not on this machine: download them in the Canvas view first") : modelData.description
                        current: root.action === modelData.id
                        onTapped: root.action = modelData.id
                    }
                }
            }

            Caption { text: qsTr("Prompt"); visible: root.actionRow !== null && root.actionRow.needsPrompt }
            ThemedTextField {
                id: promptField
                objectName: "canvasBatchPrompt"
                visible: root.actionRow !== null && root.actionRow.needsPrompt
                Layout.fillWidth: true
                placeholderText: qsTr("What to put in each picture")
                Accessible.name: qsTr("Prompt")
            }

            Caption { text: qsTr("Pictures") }
            RowLayout {
                Layout.fillWidth: true
                spacing: 2 * root.look.unit
                ThemedButton { objectName: "canvasBatchChooseIn"; text: qsTr("Choose folder…"); onClicked: root.chooseFolder(true) }
                Label {
                    objectName: "canvasBatchInLabel"
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    color: root.look.label
                    text: root.inFolder === "" ? qsTr("No folder chosen")
                          : root.pictures < 0 ? qsTr("The folder cannot be read")
                          : qsTr("%n pictures", "", root.pictures) + " · " + root.pathText(root.inFolder)
                }
            }

            Caption { text: qsTr("Save to") }
            RowLayout {
                Layout.fillWidth: true
                spacing: 2 * root.look.unit
                ThemedButton { objectName: "canvasBatchChooseOut"; text: qsTr("Choose folder…"); onClicked: root.chooseFolder(false) }
                Label {
                    objectName: "canvasBatchOutLabel"
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    color: root.look.label
                    text: root.outFolder === "" ? qsTr("No folder chosen") : root.pathText(root.outFolder)
                }
            }

            Caption { text: qsTr("Format") }
            Flow {
                Layout.fillWidth: true
                spacing: root.look.unit
                Repeater {
                    model: [{id: "png", label: qsTr("PNG")}, {id: "jpg", label: qsTr("JPEG")}, {id: "webp", label: qsTr("WebP")}, {id: "ora", label: qsTr("Layers (.ora)")}]
                    Choice {
                        required property var modelData
                        objectName: "canvasBatchFormat-" + modelData.id
                        text: modelData.label
                        current: root.format === modelData.id
                        onTapped: root.format = modelData.id
                    }
                }
            }
        }

        Label {
            objectName: "canvasBatchNote"
            visible: !root.showsResults
            Layout.fillWidth: true
            color: root.look.label
            font.pixelSize: root.look.captionSize
            wrapMode: Text.Wrap
            text: qsTr("Each picture becomes a canvas, the action runs on all of it, and the result is saved as a new file; a name that is taken gets -2, -3, and nothing is replaced. It runs in the background.")
        }

        // The pictures and how each went.
        ProgressBar {
            visible: root.showsResults
            Layout.fillWidth: true
            from: 0
            to: Math.max(1, root.batch ? root.batch.total : 1)
            value: root.batch ? root.batch.done + (root.batch.progress > 0 ? root.batch.progress : 0) : 0
            Accessible.name: qsTr("Batch progress")
        }
        Label {
            objectName: "canvasBatchSummary"
            visible: root.showsResults
            Layout.fillWidth: true
            text: root.batch && root.batch.finished ? root.batch.summary : qsTr("%1 of %2 pictures…").arg(root.batch ? root.batch.done : 0).arg(root.batch ? root.batch.total : 0)
            wrapMode: Text.Wrap
        }
        ListView {
            id: results
            objectName: "canvasBatchResults"
            visible: root.showsResults
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 28 * root.look.unit)
            clip: true
            model: root.batch ? root.batch.results : []
            ScrollBar.vertical: FugueScrollBar { look: root.look }
            delegate: ColumnLayout {
                id: resultRow
                required property var modelData
                width: ListView.view.width
                spacing: 0
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2 * root.look.unit
                    Label { text: resultRow.modelData.name; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    Label {
                        objectName: "canvasBatchStatus"
                        text: resultRow.modelData.status === "ok" ? qsTr("Done")
                              : resultRow.modelData.status === "failed" ? qsTr("Failed")
                              : resultRow.modelData.status === "cancelled" ? qsTr("Cancelled")
                              : resultRow.modelData.status === "running" ? qsTr("Running…") : qsTr("Waiting")
                        color: resultRow.modelData.status === "failed" ? root.look.error : root.look.label
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: errLabel.text !== ""
                    spacing: 2 * root.look.unit
                    Label {
                        id: errLabel
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: resultRow.modelData.error
                        color: root.look.error
                        font.pixelSize: root.look.captionSize
                        wrapMode: Text.Wrap
                    }
                    KitCopyButton {
                        objectName: "batchRowErrorCopy"
                        look: root.look
                        Layout.alignment: Qt.AlignTop
                        copyText: resultRow.modelData.error
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: errLabelAll.text !== ""
            spacing: 2 * root.look.unit
            Label {
                id: errLabelAll
                objectName: "canvasBatchError"
                Layout.fillWidth: true
                visible: text !== ""
                text: root.problem
                color: root.look.error
                wrapMode: Text.Wrap
            }
            KitCopyButton {
                objectName: "canvasBatchErrorCopy"
                look: root.look
                Layout.alignment: Qt.AlignTop
                copyText: root.problem
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 2 * root.look.unit
            ThemedButton {
                objectName: "canvasBatchCancel"
                danger: true
                visible: root.batch !== null && root.batch.busy
                text: qsTr("Cancel batch")
                onClicked: root.batch.cancel()
            }
            ThemedButton {
                id: closeButton
                objectName: "canvasBatchClose"
                text: root.batch !== null && root.batch.busy ? qsTr("Hide") : qsTr("Close")
                highlighted: root.batch !== null && root.batch.finished
                onClicked: root.close()
            }
            ThemedButton {
                objectName: "canvasBatchAgain"
                visible: root.batch !== null && root.batch.finished
                text: qsTr("New batch")
                onClicked: root.batch.reset()
            }
            ThemedButton {
                id: runButton
                objectName: "canvasBatchRun"
                visible: !root.showsResults
                text: qsTr("Run")
                highlighted: true
                enabled: root.ready
                onClicked: root.run()
            }
        }
    }

    function pathText(path) { return String(path).replace(/^file:\/\//, "") }
    // Which folder the picker is for: true for the pictures, false for the results.
    property bool choosingIn: true
    function chooseFolder(forPictures) {
        choosingIn = forPictures
        folderPicker.title = forPictures ? qsTr("Choose the folder of pictures") : qsTr("Choose the folder for the results")
        folderPicker.currentFolder = StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        folderPicker.open()
    }
    FilePicker {
        id: folderPicker
        objectName: "canvasBatchFolderPicker"
        fileMode: PortalFileDialog.Directory
        onAccepted: {
            const path = selectedFolder.toString()
            if (root.choosingIn)
                root.inFolder = path
            else
                root.outFolder = path
        }
    }
    Component.onDestruction: folderPicker.close()
}
