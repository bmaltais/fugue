pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../menu"
import "../canvas"

// Everything the workflows show besides the tabs (spec 11): the file pickers (the desktop
// portal, Qt's own dialog when it is not there), the save/discard/cancel question, the trust question, the offer
// to recover, the banner for what went wrong, and the drop area for files. Reads
// App.workflows and App.theme. Styled from theme.look (docs/canvas-look.md):
// dialogs are level 3 surfaces over the scrim, notices sit on the popup fill.
Item {
    id: root

    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    readonly property CanvasLook look: App.theme.look
    // The canvas, to turn a drop point into world coordinates.
    property FugueCanvas canvasView
    property real noticeInset: 0 // the rail at the window's left edge, which the notice keeps clear of
    property alias closeQuestion: closeAsk
    property alias trustQuestion: trustAsk
    property alias recoveryQuestion: recoverAsk

    // The pickers ask the desktop portal (FilePicker, src/menu/FilePicker.qml: org.freedesktop.portal.FileChooser).
    // Qt's FileDialog is not native here: Omarchy's platform theme (qtengine) has no file dialog, so it
    // would always show Qt's own. That one is the fallback, where the portal is not running or fails.
    FilePicker {
        id: openDialog
        objectName: "openDialog"
        title: qsTr("Open a workflow")
        fileMode: PortalFileDialog.OpenFiles
        currentFolder: App.workflows.suggestedFolder
        nameFilters: [qsTr("Fugue workflows, packages, apps and images with one (*.fugue *.fuguepack *.fugueapp *.png *.webp)"), qsTr("All files (*)")]
        onAccepted: App.workflows.openUrls(selectedFiles)
    }

    FilePicker {
        id: saveDialog
        objectName: "saveDialog"
        title: qsTr("Save the workflow")
        fileMode: PortalFileDialog.SaveFile
        currentFolder: App.workflows.suggestedFolder
        defaultSuffix: "fugue"
        nameFilters: [qsTr("Fugue workflows (*.fugue)")]
        onAccepted: App.workflows.saveAs(selectedFile.toString())
        onRejected: App.workflows.cancelPendingClose()
    }

    // File > Export for sharing (#91): where the .fuguepack goes; its preview is the last picture made.
    FilePicker {
        id: exportDialog
        objectName: "exportDialog"
        title: qsTr("Export for sharing")
        fileMode: PortalFileDialog.SaveFile
        currentFolder: App.workflows.suggestedFolder
        defaultSuffix: "fuguepack"
        nameFilters: [qsTr("Fugue packages (*.fuguepack)")]
        onAccepted: App.workflows.exportShare(selectedFile.toString(), App.runs ? App.runs.lastPicture() : "")
    }

    FilePicker {
        id: compiledDialog
        objectName: "compiledDialog"
        title: qsTr("Export the compiled graph")
        fileMode: PortalFileDialog.SaveFile
        currentFolder: App.workflows.suggestedFolder
        defaultSuffix: "json"
        nameFilters: [qsTr("JSON (*.json)")]
        onAccepted: App.workflows.exportCompiled(selectedFile.toString())
    }

    Connections {
        target: App.workflows
        function onOpenDialogRequested() { openDialog.open() }
        function onExportShareRequested() {
            // An app is shared as a bundle (#394), anything else as a package.
            const app = App.workflows.shareSuffix() === "fugueapp"
            exportDialog.defaultSuffix = app ? "fugueapp" : "fuguepack"
            exportDialog.nameFilters = app ? [qsTr("Fugue apps (*.fugueapp)")] : [qsTr("Fugue packages (*.fuguepack)")]
            exportDialog.open()
        }
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
    Banner {
        id: notice
        objectName: "workflowNotice"
        readonly property var downloads: App.workflows.downloads
        visible: App.workflows.notice.length > 0 || downloads.busy
        look: root.look
        tone: App.workflows.getReadyOffer || downloads.busy ? "moss" : "slate" // what a workflow lacks and its downloads are Get ready's area
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 2 * root.look.unit; leftMargin: root.noticeInset + 2 * root.look.unit }
        ColumnLayout {
            id: noticeColumn
            width: parent.width
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
                // One button for everything the workflow lacks (#379): packs, their trust and environments, and models,
                // asked once on the sheet, with where models go.
                Loader {
                    active: App.workflows.getReadyOffer // made only when shown: the window keeps few items (#5)
                    sourceComponent: ThemedButton {
                        objectName: "getReady"
                        highlighted: true
                        text: qsTr("Get ready…")
                        onClicked: App.workflows.readiness.show()
                    }
                }
                ToolButton {
                    // The Lucide x every other close and dismiss draws (canvas-look 16), not a text "×".
                    contentItem: Glyph { look: App.theme.look; name: "x"; color: App.theme.look.label; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
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
        onOpened: closeSave.forceActiveFocus() // Enter takes the primary
        ColumnLayout {
            anchors.fill: parent
            spacing: 4 * root.look.unit
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Save the changes to %1 before closing?").arg(closeAsk.title)
            }
            // Discard loses the changes: the danger tint, alone at the far side; Cancel, then Save (the primary) at the right.
            QuestionAnswers {
                leadingSpacer: false
                ThemedButton {
                    objectName: "closeDiscard"
                    text: qsTr("Discard")
                    danger: true
                    onClicked: { closeAsk.close(); App.workflows.discardAndClose(closeAsk.index) }
                }
                Item { Layout.fillWidth: true }
                ThemedButton { objectName: "closeCancel"; text: qsTr("Cancel"); onClicked: closeAsk.close() }
                ThemedButton {
                    id: closeSave
                    objectName: "closeSave"
                    text: qsTr("Save")
                    highlighted: true
                    onClicked: { closeAsk.close(); App.workflows.saveAndClose(closeAsk.index) }
                }
            }
        }
    }

    // The sheet asks about packs it installs together with everything else (#379), so this waits for it.
    // Made the first time it opens, and kept: the window keeps few items (#5).
    property var getReadySheet: null
    Component {
        id: getReadyComponent
        GetReadySheet {}
    }
    Connections {
        target: App.workflows.readiness
        function onChanged() {
            if (App.workflows.readiness.open && !root.getReadySheet)
                root.getReadySheet = getReadyComponent.createObject(root)
        }
    }

    Question {
        id: trustAsk
        objectName: "trustAsk"
        visible: App.workflows.pendingTrust.length > 0 && !App.workflows.getReadyOffer && !App.workflows.readiness.open
        closePolicy: Popup.NoAutoClose
        ColumnLayout {
            anchors.fill: parent
            spacing: 3 * root.look.unit
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
            QuestionAnswers {
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
            spacing: 3 * root.look.unit
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
            QuestionAnswers {
                spacing: 4 * root.look.unit // Discard is the destructive one: twice the row's gap before Recover
                ThemedButton {
                    objectName: "recoverDiscard"
                    text: qsTr("Discard")
                    danger: true
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
