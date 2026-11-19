import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Everything the workflows show besides the tabs (spec 11): the file pickers (the desktop
// portal when there is one), the save/discard/cancel question, the trust question, the offer
// to recover, the banner for what went wrong, and the drop area for files. Needs the
// `workflows` and `theme` context properties. Plain controls; a design pass restyles them.
Item {
    id: root

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
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
        currentFolder: workflows.suggestedFolder
        nameFilters: [qsTr("Fugue workflows and images with one (*.fugue *.png *.webp)"), qsTr("All files (*)")]
        onAccepted: workflows.openUrls(selectedFiles)
    }

    FileDialog {
        id: saveDialog
        objectName: "saveDialog"
        title: qsTr("Save the workflow")
        fileMode: FileDialog.SaveFile
        currentFolder: workflows.suggestedFolder
        defaultSuffix: "fugue"
        nameFilters: [qsTr("Fugue workflows (*.fugue)")]
        onAccepted: workflows.saveAs(selectedFile.toString())
        onRejected: workflows.cancelPendingClose()
    }

    Connections {
        target: workflows
        function onOpenDialogRequested() { openDialog.open() }
        function onSaveAsRequested() { saveDialog.open() }
        function onCloseConfirmationRequested(index, title) {
            closeAsk.index = index
            closeAsk.title = title
            closeAsk.open()
        }
    }

    // A file dropped on the window: a .fugue, or an output with its workflow embedded.
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: (drop) => { if (drop.hasUrls) workflows.openUrls(drop.urls) }
    }

    // What went wrong, or what the opened workflow lacks.
    Rectangle {
        id: notice
        objectName: "workflowNotice"
        visible: workflows.notice.length > 0
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: visible ? noticeRow.implicitHeight + 16 : 0
        color: theme.panel
        border.color: theme.border
        RowLayout {
            id: noticeRow
            anchors { fill: parent; margins: 8 }
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: theme.foreground
                text: workflows.notice
            }
            ToolButton {
                text: "×"
                focusPolicy: Qt.NoFocus
                Accessible.name: qsTr("Dismiss")
                onClicked: workflows.dismissNotice()
            }
        }
    }

    component Question: Popup {
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        padding: 24
        width: Math.min(root.width - 48, 520)
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
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
                Button { text: qsTr("Cancel"); onClicked: closeAsk.close() }
                Button {
                    objectName: "closeDiscard"
                    text: qsTr("Discard")
                    onClicked: { closeAsk.close(); workflows.discardAndClose(closeAsk.index) }
                }
                Button {
                    objectName: "closeSave"
                    text: qsTr("Save")
                    highlighted: true
                    onClicked: { closeAsk.close(); workflows.saveAndClose(closeAsk.index) }
                }
            }
        }
    }

    Question {
        id: trustAsk
        objectName: "trustAsk"
        visible: workflows.pendingTrust.length > 0
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
                model: workflows.pendingTrust
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
                Button {
                    objectName: "trustLater"
                    text: qsTr("Not now")
                    onClicked: workflows.dismissPendingTrust()
                }
                Button {
                    objectName: "trustAllow"
                    text: qsTr("Allow")
                    highlighted: true
                    onClicked: workflows.allowPendingTrust()
                }
            }
        }
    }

    Question {
        id: recoverAsk
        objectName: "recoverAsk"
        visible: workflows.recoverable.length > 0
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
                model: workflows.recoverable
                delegate: Label {
                    required property var modelData
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: modelData.title + (modelData.path ? "  " + modelData.path : "")
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    objectName: "recoverDiscard"
                    text: qsTr("Discard")
                    onClicked: workflows.discardRecovery()
                }
                Button {
                    objectName: "recoverOpen"
                    text: qsTr("Recover")
                    highlighted: true
                    onClicked: workflows.recover()
                }
            }
        }
    }
}
