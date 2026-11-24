pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../menu"
import "../canvas"

// The "Get ready" sheet (spec 6.6, 8.9, 11.10; #379): what the shown workflow needs (packs to install and trust, their
// environments, the missing models) as one list and one yes, then a row of progress each. The list, the checks and the work
// are App.workflows.readiness (C++, workflow/readiness_sheet.h, the same plan the CLI's `prepare` shows); this only shows it.
// Nothing happens until "Install".
Question {
    id: sheet
    objectName: "getReadySheet"
    readonly property var sheetModel: App.workflows.readiness
    readonly property var view: sheetModel.view
    readonly property string phase: sheetModel.phase
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    visible: sheetModel.open
    closePolicy: Popup.NoAutoClose
    width: Math.min((Overlay.overlay ? Overlay.overlay.width : 640) - 12 * look.unit, 640)
    // Rows opened for their detail, by "pack:<id>" or "model:<name>".
    property var expanded: ({})
    function toggle(key) {
        const copy = Object.assign({}, expanded)
        copy[key] = !copy[key]
        expanded = copy
    }
    function stateText(state) {
        return state === "done" ? qsTr("Done") : state === "running" ? qsTr("Working…") : state === "failed" ? qsTr("Failed")
             : state === "skipped" ? qsTr("Not done") : ""
    }

    FilePicker {
        id: destinationPicker
        objectName: "getReadyFolderDialog"
        title: qsTr("Pick a folder for models")
        fileMode: PortalFileDialog.Directory
        onAccepted: sheet.sheetModel.pickDestination(selectedFolder)
    }
    Menu {
        id: destinationMenu
        objectName: "getReadyDestinations"
        Repeater {
            model: sheet.view.destinations
            MenuItem {
                required property var modelData
                text: modelData.title
                checkable: true
                checked: modelData.path === sheet.view.destination
                onTriggered: sheet.sheetModel.changeDestination(modelData.path)
            }
        }
        MenuItem {
            objectName: "getReadyPickFolder"
            text: qsTr("Pick a folder…")
            onTriggered: destinationPicker.open()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 3 * sheet.look.unit
        LayoutMirroring.enabled: sheet.rightToLeft
        LayoutMirroring.childrenInherit: true

        Label {
            objectName: "getReadyHeadline"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.bold: true
            text: sheet.view.headline
        }
        Label {
            visible: sheet.view.packs.length > 0
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: sheet.look.label
            text: qsTr("A pack's nodes run as you, with no sandbox. Nothing is installed, trusted, set up or downloaded until you say yes here.")
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(rows.implicitHeight, 280)
            clip: true
            ColumnLayout {
                id: rows
                width: sheet.width - 12 * sheet.look.unit - 16
                spacing: 2 * sheet.look.unit
                Repeater {
                    model: sheet.view.packs
                    ColumnLayout {
                        id: packRow
                        required property var modelData
                        readonly property string key: "pack:" + modelData.id
                        objectName: "getReadyPack-" + modelData.id
                        Layout.fillWidth: true
                        spacing: sheet.look.unit
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: (packRow.modelData.install ? qsTr("Install and trust the pack %1 %2") : qsTr("Trust the pack %1 %2")).arg(packRow.modelData.name).arg(packRow.modelData.version)
                                      + (packRow.modelData.setup !== "" ? qsTr(", and set up its environment") : "")
                            }
                            Label {
                                color: packRow.modelData.state === "failed" ? sheet.look.error : sheet.look.label
                                text: sheet.stateText(packRow.modelData.state)
                            }
                            ToolButton {
                                objectName: "getReadyDetails-" + packRow.modelData.id
                                visible: !packRow.modelData.pending
                                text: sheet.expanded[packRow.key] ? qsTr("Hide details") : qsTr("Details")
                                focusPolicy: Qt.NoFocus
                                onClicked: sheet.toggle(packRow.key)
                            }
                        }
                        Label {
                            objectName: "getReadySummary-" + packRow.modelData.id
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: sheet.look.label
                            text: packRow.modelData.summary
                        }
                        Label {
                            visible: sheet.expanded[packRow.key] === true
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: sheet.look.label
                            text: (packRow.modelData.source !== "" ? qsTr("From: %1").arg(packRow.modelData.source) + "\n" : "")
                                  + qsTr("Runs as: %1").arg(packRow.modelData.runtime) + "\n"
                                  + qsTr("Declares: %1").arg(packRow.modelData.permissions)
                                  + (packRow.modelData.setup !== "" ? "\n" + qsTr("Setup command: %1").arg(packRow.modelData.setup)
                                     + (packRow.modelData.setupSize !== "" ? " (" + qsTr("downloads %1").arg(packRow.modelData.setupSize) + ")" : "")
                                     + (packRow.modelData.setupNote !== "" ? "\n" + packRow.modelData.setupNote : "") : "")
                        }
                        Label {
                            visible: packRow.modelData.detail !== "" && packRow.modelData.state === "failed"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: sheet.look.error
                            text: packRow.modelData.detail
                        }
                    }
                }
                Repeater {
                    model: sheet.view.models
                    ColumnLayout {
                        id: modelRow
                        required property var modelData
                        readonly property string key: "model:" + modelData.name
                        objectName: "getReadyModel-" + modelData.name
                        Layout.fillWidth: true
                        spacing: sheet.look.unit
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                                text: qsTr("Download %1 (%2)").arg(modelRow.modelData.name).arg(modelRow.modelData.size)
                                      + (modelRow.modelData.typeFrom !== "" ? " · " + modelRow.modelData.typeFrom : "")
                            }
                            Label {
                                color: modelRow.modelData.state === "failed" ? sheet.look.error : sheet.look.label
                                text: sheet.stateText(modelRow.modelData.state)
                            }
                            ToolButton {
                                text: sheet.expanded[modelRow.key] ? qsTr("Hide details") : qsTr("Details")
                                focusPolicy: Qt.NoFocus
                                onClicked: sheet.toggle(modelRow.key)
                            }
                        }
                        ProgressBar {
                            visible: modelRow.modelData.state === "running"
                            Layout.fillWidth: true
                            from: 0
                            to: 1
                            indeterminate: modelRow.modelData.fraction < 0
                            value: Math.max(0, modelRow.modelData.fraction)
                        }
                        Label {
                            visible: sheet.expanded[modelRow.key] === true
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: sheet.look.label
                            text: qsTr("From: %1").arg(modelRow.modelData.host) + "\n"
                                  + (modelRow.modelData.license !== "" ? qsTr("Licence: %1").arg(modelRow.modelData.license) + "\n" : "")
                                  + qsTr("Into: %1").arg(modelRow.modelData.folder)
                        }
                        Label {
                            visible: modelRow.modelData.detail !== "" && modelRow.modelData.state === "failed"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: sheet.look.error
                            text: modelRow.modelData.detail
                        }
                    }
                }
            }
        }

        // Where models go (8.9): the current folder with free space against the download size, and Change….
        RowLayout {
            objectName: "getReadyDestination"
            visible: sheet.view.hasModels
            Layout.fillWidth: true
            spacing: 2 * sheet.look.unit
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Models go into: %1").arg(sheet.view.destinationTitle)
                      + (sheet.view.freeText !== "" ? " · " + sheet.view.freeText : "")
                      + (sheet.view.sizeText !== "" ? " · " + qsTr("needs %1").arg(sheet.view.sizeText) : "")
            }
            ThemedButton {
                objectName: "getReadyChange"
                visible: sheet.phase === "review"
                text: qsTr("Change…")
                onClicked: destinationMenu.popup()
            }
        }
        Label {
            objectName: "getReadyLowSpace"
            visible: sheet.view.hasModels && sheet.view.lowSpace
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: sheet.look.error
            text: qsTr("There is not enough free space in that folder for the download. Change it to another folder before you go on.")
        }
        Repeater {
            model: sheet.view.problems
            Label {
                required property string modelData
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: sheet.look.label
                text: modelData
            }
        }
        // A pack fetched after the yes declares access the list could not show: asked once, for that row alone.
        ColumnLayout {
            objectName: "getReadyQuestion"
            visible: sheet.view.question.text !== undefined
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.bold: true
                text: sheet.view.question.text ?? ""
            }
            QuestionAnswers {
                ThemedButton {
                    objectName: "getReadySkip"
                    text: qsTr("Skip")
                    onClicked: sheet.sheetModel.answer(false)
                }
                ThemedButton {
                    objectName: "getReadyAllow"
                    text: qsTr("Allow")
                    onClicked: sheet.sheetModel.answer(true)
                }
            }
        }
        Label {
            objectName: "getReadyMessage"
            visible: sheet.view.message !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: sheet.view.ok ? sheet.look.text : sheet.look.error
            text: sheet.view.message + (sheet.phase !== "finished" || sheet.view.ok || sheet.view.retry.length === 0 ? "" : " " + qsTr("Not done: %1. Try again; it lists only what is left.").arg(sheet.view.retry.join(", ")))
        }

        QuestionAnswers {
            ThemedButton {
                objectName: "getReadyCancel"
                visible: sheet.phase !== "finished"
                text: sheet.phase === "running" ? qsTr("Stop") : qsTr("Cancel")
                onClicked: sheet.sheetModel.cancel()
            }
            ThemedButton {
                objectName: "getReadyClose"
                visible: sheet.phase === "finished"
                text: qsTr("Close")
                onClicked: sheet.sheetModel.close()
            }
            ThemedButton {
                objectName: "getReadyAgain"
                visible: sheet.phase === "finished" && !sheet.view.ok
                text: qsTr("Try again")
                onClicked: sheet.sheetModel.show()
            }
            ThemedButton {
                objectName: "getReadyAll"
                visible: sheet.phase === "review" && !sheet.view.empty
                enabled: !sheet.view.lowSpace
                highlighted: true
                text: qsTr("Install")
                onClicked: sheet.sheetModel.start()
            }
        }
    }
}
