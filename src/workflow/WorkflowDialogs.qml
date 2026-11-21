pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Fugue
import "../canvas"

// Everything the workflows show besides the tabs (spec 11): the file pickers (the desktop
// portal when there is one), the save/discard/cancel question, the trust question, the offer
// to recover, the banner for what went wrong, and the drop area for files. Reads
// App.workflows and App.theme. Styled from theme.look (docs/canvas-look.md):
// dialogs are level 3 surfaces over the scrim, notices sit on the popup fill.
Item {
    id: root

    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    readonly property CanvasLook look: App.theme.look
    // The canvas, to turn a drop point into world coordinates.
    property FugueCanvas canvasView
    property alias closeQuestion: closeAsk
    property alias trustQuestion: trustAsk
    property alias recoveryQuestion: recoverAsk

    // The pickers use the platform's native dialog, which is the desktop portal's on Omarchy;
    // Qt's own is the fallback where there is none.
    FileDialog {
        id: openDialog
        objectName: "openDialog"
        title: qsTr("Open a workflow")
        fileMode: FileDialog.OpenFiles
        currentFolder: App.workflows.suggestedFolder
        nameFilters: [qsTr("Fugue workflows, packages and images with one (*.fugue *.zip *.png *.webp)"), qsTr("All files (*)")]
        onAccepted: App.workflows.openUrls(selectedFiles)
    }

    FileDialog {
        id: saveDialog
        objectName: "saveDialog"
        title: qsTr("Save the workflow")
        fileMode: FileDialog.SaveFile
        currentFolder: App.workflows.suggestedFolder
        defaultSuffix: "fugue"
        nameFilters: [qsTr("Fugue workflows (*.fugue)")]
        onAccepted: App.workflows.saveAs(selectedFile.toString())
        onRejected: App.workflows.cancelPendingClose()
    }

    // File > Export for sharing (#91): where the .zip goes; its preview is the last picture made.
    FileDialog {
        id: exportDialog
        objectName: "exportDialog"
        title: qsTr("Export for sharing")
        fileMode: FileDialog.SaveFile
        currentFolder: App.workflows.suggestedFolder
        defaultSuffix: "zip"
        nameFilters: [qsTr("Fugue packages (*.zip)")]
        onAccepted: App.workflows.exportShare(selectedFile.toString(), App.runs ? App.runs.lastPicture() : "")
    }

    FileDialog {
        id: compiledDialog
        objectName: "compiledDialog"
        title: qsTr("Export the compiled graph")
        fileMode: FileDialog.SaveFile
        currentFolder: App.workflows.suggestedFolder
        defaultSuffix: "json"
        nameFilters: [qsTr("JSON (*.json)")]
        onAccepted: App.workflows.exportCompiled(selectedFile.toString())
    }

    Connections {
        target: App.workflows
        function onOpenDialogRequested() { openDialog.open() }
        function onExportShareRequested() { exportDialog.open() }
        function onExportCompiledRequested() { compiledDialog.open() }
        function onSaveAsRequested() { saveDialog.open() }
        function onCloseConfirmationRequested(index, title) {
            closeAsk.index = index
            closeAsk.title = title
            closeAsk.open()
        }
    }

    // Files dropped on the window: a .fugue or an output with its workflow embedded opens; an
    // image goes into a Load Image node, the one under the pointer when there is one.
    DropArea {
        id: dropArea
        objectName: "windowDropArea"
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: (drop) => {
            if (!drop.hasUrls)
                return
            const p = root.canvasView ? root.canvasView.mapFromItem(dropArea, drop.x, drop.y) : Qt.point(0, 0)
            const w = root.canvasView ? root.canvasView.toWorld(p.x, p.y) : p
            App.workflows.dropUrls(drop.urls, w.x, w.y)
        }
    }

    // What went wrong, or what the opened workflow lacks; its missing models' Download button
    // (#89), and the models downloading.
    Rectangle {
        id: notice
        objectName: "workflowNotice"
        readonly property var downloads: App.workflows.downloads
        visible: App.workflows.notice.length > 0 || downloads.busy
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: visible ? noticeColumn.implicitHeight + 16 : 0
        color: root.look.popup
        border.color: root.look.hairline
        ColumnLayout {
            id: noticeColumn
            anchors { fill: parent; margins: 8 }
            spacing: 16 // two chunks: the line and its button, then the download under way
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            RowLayout {
                id: noticeRow
                Layout.fillWidth: true
                visible: App.workflows.notice.length > 0
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: root.look.text
                    text: App.workflows.notice
                }
                // Where they go, when more than one model folder can take them; remembered. Made
                // only when shown, as the progress row below: the window keeps few items (#5).
                Loader {
                    active: App.workflows.downloadOffer !== "" && App.workflows.downloadRoots.length > 1
                    sourceComponent: ComboBox {
                        objectName: "downloadInto"
                        model: App.workflows.downloadRoots.map(r => qsTr("Into %1").arg(r.title))
                        currentIndex: Math.max(0, App.workflows.downloadRoots.findIndex(r => r.path === App.workflows.downloadRoot))
                        onActivated: index => App.workflows.setDownloadRoot(App.workflows.downloadRoots[index].path)
                        Accessible.name: qsTr("Download into")
                    }
                }
                // A pack its nodes come from, where the workflow says it can be had (#91).
                Loader {
                    active: App.workflows.packOffer.id !== undefined
                    sourceComponent: ThemedButton {
                        objectName: "installPack"
                        text: App.workflows.packOffer.text
                        onClicked: App.workflows.installPack()
                    }
                }
                Loader {
                    active: App.workflows.downloadOffer !== ""
                    sourceComponent: ThemedButton {
                        objectName: "downloadModels"
                        highlighted: true
                        text: App.workflows.downloadOffer
                        onClicked: App.workflows.downloadMissing()
                    }
                }
                ToolButton {
                    text: "×"
                    focusPolicy: Qt.NoFocus
                    Accessible.name: qsTr("Dismiss")
                    onClicked: App.workflows.dismissNotice()
                }
            }
            Loader {
                Layout.fillWidth: true
                active: notice.downloads.busy
                sourceComponent: RowLayout {
                    objectName: "downloadProgress"
                    spacing: 8
                    Label {
                        color: root.look.text
                        text: qsTr("Downloading %1").arg(notice.downloads.label)
                        elide: Text.ElideMiddle
                        Layout.maximumWidth: noticeColumn.width / 2
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        from: 0
                        to: 1
                        indeterminate: notice.downloads.fraction < 0
                        value: Math.max(0, notice.downloads.fraction)
                    }
                    Label {
                        color: root.look.label
                        text: notice.downloads.progressText
                    }
                    ThemedButton {
                        objectName: "downloadCancel"
                        Layout.leftMargin: 8 // Destructive: twice the row's gap before it
                        text: qsTr("Cancel")
                        onClicked: notice.downloads.cancel()
                    }
                }
            }
        }
    }


    Question {
        id: closeAsk
        objectName: "closeAsk"
        property int index: -1
        property string title: ""
        ColumnLayout {
            anchors.fill: parent
            spacing: 16
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Save the changes to %1 before closing?").arg(closeAsk.title)
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                ThemedButton { text: qsTr("Cancel"); onClicked: closeAsk.close() }
                ThemedButton {
                    objectName: "closeDiscard"
                    text: qsTr("Discard")
                    onClicked: { closeAsk.close(); App.workflows.discardAndClose(closeAsk.index) }
                }
                ThemedButton {
                    objectName: "closeSave"
                    text: qsTr("Save")
                    highlighted: true
                    onClicked: { closeAsk.close(); App.workflows.saveAndClose(closeAsk.index) }
                }
            }
        }
    }

    Question {
        id: trustAsk
        objectName: "trustAsk"
        visible: App.workflows.pendingTrust.length > 0
        closePolicy: Popup.NoAutoClose
        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.bold: true
                text: qsTr("This workflow uses nodes from packs you have not allowed")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("A command node is a program that runs as you, with no sandbox. Nothing from these packs runs until you allow it.")
            }
            Repeater {
                model: App.workflows.pendingTrust
                delegate: Label {
                    required property var modelData
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: modelData.id + (modelData.version ? " " + modelData.version : "")
                          + (modelData.source ? "\n" + modelData.source : "")
                          + "\n" + modelData.types.join(", ")
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                ThemedButton {
                    objectName: "trustLater"
                    text: qsTr("Not now")
                    onClicked: App.workflows.dismissPendingTrust()
                }
                ThemedButton {
                    objectName: "trustAllow"
                    text: qsTr("Allow")
                    highlighted: true
                    onClicked: App.workflows.allowPendingTrust()
                }
            }
        }
    }

    Question {
        id: recoverAsk
        objectName: "recoverAsk"
        visible: App.workflows.recoverable.length > 0
        closePolicy: Popup.NoAutoClose
        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.bold: true
                text: qsTr("Fugue closed with unsaved work. Recover it?")
            }
            Repeater {
                model: App.workflows.recoverable
                delegate: Label {
                    required property var modelData
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: modelData.title + (modelData.path ? "  " + modelData.path : "")
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                ThemedButton {
                    objectName: "recoverDiscard"
                    text: qsTr("Discard")
                    onClicked: App.workflows.discardRecovery()
                }
                ThemedButton {
                    objectName: "recoverOpen"
                    text: qsTr("Recover")
                    highlighted: true
                    onClicked: App.workflows.recover()
                }
            }
        }
    }
}
