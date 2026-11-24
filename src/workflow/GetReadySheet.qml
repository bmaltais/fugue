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
    property var sheetModel: App.workflows.readiness // not readonly: the picture tests give it a made-up plan
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

        // The title, then what will happen in one line with the totals.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: sheet.look.unit
            Text {
                objectName: "getReadyTitle"
                Layout.fillWidth: true
                text: qsTr("Get ready")
                wrapMode: Text.Wrap
                color: sheet.look.text
                font.pixelSize: sheet.look.overlayTitleSize
                font.weight: Font.DemiBold
            }
            Text {
                objectName: "getReadyHeadline"
                Layout.fillWidth: true
                text: sheet.view.headline
                wrapMode: Text.Wrap
                color: sheet.look.label
                font.pixelSize: sheet.look.textSize
            }
        }

        // What it takes, in groups. It scrolls (up and down only) when it is taller than the window can spare; a row that is
        // too long is cut at its end, with the whole line as its tooltip, so there is no sideways bar.
        Flickable {
            id: list
            objectName: "getReadyList"
            Layout.fillWidth: true
            Layout.topMargin: sheet.look.unit
            Layout.preferredHeight: Math.min(rows.implicitHeight, 0.45 * (Overlay.overlay ? Overlay.overlay.height : 800))
            clip: true
            contentWidth: width
            contentHeight: rows.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FugueScrollBar { look: sheet.look }

            ColumnLayout {
                id: rows
                width: list.width - (list.ScrollBar.vertical as FugueScrollBar).breadth
                spacing: 0

                Text {
                    visible: sheet.view.packs.length > 0
                    Layout.leftMargin: 2 * sheet.look.unit
                    Layout.bottomMargin: 0.5 * sheet.look.unit
                    text: qsTr("Packs")
                    color: sheet.look.label
                    font.pixelSize: sheet.look.labelSize
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: sheet.view.packs
                    GetReadyRow {
                        id: packRow
                        required property var modelData
                        readonly property string key: "pack:" + modelData.id
                        objectName: "getReadyPack-" + modelData.id
                        Layout.fillWidth: true
                        look: sheet.look
                        glyph: "boxes"
                        title: modelData.name + " " + modelData.version
                        subtitle: modelData.pending ? modelData.summary
                                  : (modelData.install ? qsTr("Install and trust") : qsTr("Trust"))
                                    + (modelData.source !== "" ? " · " + qsTr("from %1").arg(modelData.source) : "")
                        more: modelData.pending ? ""
                              : qsTr("Runs as: %1").arg(modelData.runtime) + "\n" + qsTr("Declares: %1").arg(modelData.permissions)
                        open: sheet.expanded[key] === true
                        stateText: sheet.stateText(modelData.state)
                        failed: modelData.state === "failed"
                        error: failed ? modelData.detail : ""
                        onToggled: sheet.toggle(key)
                    }
                }

                // A pack's environment is part of its row's work, so it shows the same state.
                Text {
                    visible: envs.count > 0
                    Layout.topMargin: 2 * sheet.look.unit
                    Layout.leftMargin: 2 * sheet.look.unit
                    Layout.bottomMargin: 0.5 * sheet.look.unit
                    text: qsTr("Environments")
                    color: sheet.look.label
                    font.pixelSize: sheet.look.labelSize
                    font.weight: Font.DemiBold
                }
                Repeater {
                    id: envs
                    model: sheet.view.packs.filter(p => p.setup !== "")
                    GetReadyRow {
                        id: envRow
                        required property var modelData
                        readonly property string key: "env:" + modelData.id
                        objectName: "getReadyEnvironment-" + modelData.id
                        Layout.fillWidth: true
                        look: sheet.look
                        glyph: "settings"
                        title: qsTr("Environment for %1").arg(modelData.name)
                        trailing: modelData.setupSize
                        subtitle: qsTr("runs %1").arg(modelData.setup)
                        more: modelData.setupNote
                        open: sheet.expanded[key] === true
                        stateText: sheet.stateText(modelData.state)
                        failed: modelData.state === "failed"
                        onToggled: sheet.toggle(key)
                    }
                }

                Text {
                    visible: sheet.view.models.length > 0
                    Layout.topMargin: 2 * sheet.look.unit
                    Layout.leftMargin: 2 * sheet.look.unit
                    Layout.bottomMargin: 0.5 * sheet.look.unit
                    text: qsTr("Models")
                    color: sheet.look.label
                    font.pixelSize: sheet.look.labelSize
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: sheet.view.models
                    GetReadyRow {
                        id: modelRow
                        required property var modelData
                        readonly property string key: "model:" + modelData.name
                        objectName: "getReadyModel-" + modelData.name
                        Layout.fillWidth: true
                        look: sheet.look
                        glyph: "download"
                        title: modelData.name
                        trailing: modelData.size
                        subtitle: [modelData.license, modelData.typeFrom].filter(t => t !== "").join(" · ")
                        more: qsTr("From: %1").arg(modelData.host) + "\n"
                              + (modelData.license !== "" ? qsTr("Licence: %1").arg(modelData.license) + "\n" : "")
                              + qsTr("Into: %1").arg(modelData.folder)
                        open: sheet.expanded[key] === true
                        running: modelData.state === "running"
                        fraction: modelData.fraction
                        stateText: modelData.state === "running" && modelData.fraction >= 0 ? Math.round(modelData.fraction * 100) + "%"
                                   : sheet.stateText(modelData.state)
                        failed: modelData.state === "failed"
                        error: failed ? modelData.detail : ""
                        onToggled: sheet.toggle(key)
                    }
                }
            }
        }

        // Where models go (8.9), a labelled row of its own: the folder, the room in it against the download, and the way to
        // change it right beside them.
        Rectangle {
            id: destination
            objectName: "getReadyDestination"
            visible: sheet.view.hasModels
            Layout.fillWidth: true
            implicitHeight: destinationRow.implicitHeight + 6 * sheet.look.unit
            radius: sheet.look.smallCorner
            color: sheet.look.well
            RowLayout {
                id: destinationRow
                anchors.fill: parent
                anchors.margins: 3 * sheet.look.unit
                anchors.leftMargin: 3 * sheet.look.unit
                spacing: 3 * sheet.look.unit
                Glyph {
                    look: sheet.look
                    name: "folder"
                    color: sheet.look.label
                    Layout.alignment: Qt.AlignVCenter
                }
                ColumnLayout {
                    Layout.fillWidth: false
                    Layout.maximumWidth: Math.max(10 * sheet.look.unit, destination.width - 14 * sheet.look.unit - change.width - 12 * sheet.look.unit)
                    spacing: 0
                    Text {
                        text: qsTr("Download models to")
                        color: sheet.look.label
                        font.pixelSize: sheet.look.labelSize
                    }
                    Text {
                        id: destinationName
                        objectName: "getReadyDestinationName"
                        Layout.fillWidth: true
                        text: sheet.view.destinationTitle
                        elide: Text.ElideMiddle
                        color: sheet.look.text
                        font.pixelSize: sheet.look.textSize
                        HoverHandler { id: destinationHover }
                        ToolTip.visible: destinationHover.hovered && sheet.view.destination !== ""
                        ToolTip.delay: sheet.look.tooltipDelay
                        ToolTip.text: sheet.view.destination
                    }
                    Text {
                        Layout.fillWidth: true
                        text: [sheet.view.freeText, sheet.view.sizeText !== "" ? qsTr("needs %1").arg(sheet.view.sizeText) : ""].filter(t => t !== "").join(" · ")
                        elide: Text.ElideRight
                        color: sheet.view.lowSpace ? sheet.look.error : sheet.look.label
                        font.pixelSize: sheet.look.labelSize
                    }
                }
                ThemedButton {
                    id: change
                    objectName: "getReadyChange"
                    visible: sheet.phase === "review"
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("Change model folder…")
                    onClicked: destinationMenu.popup()
                }
                Item { Layout.fillWidth: true }
            }
        }
        Label {
            objectName: "getReadyLowSpace"
            visible: sheet.view.hasModels && sheet.view.lowSpace
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: sheet.look.error
            font.pixelSize: sheet.look.textSize
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
                font.pixelSize: sheet.look.textSize
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

        // The pack warning, a quiet note by the yes it is about.
        RowLayout {
            objectName: "getReadyWarning"
            visible: sheet.view.packs.length > 0
            Layout.fillWidth: true
            spacing: 2 * sheet.look.unit
            Glyph {
                look: sheet.look
                name: "triangle-alert"
                color: sheet.look.label
                Layout.alignment: Qt.AlignTop
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: sheet.look.label
                font.pixelSize: sheet.look.labelSize
                text: qsTr("A pack's nodes run as you, with no sandbox. Nothing is installed, trusted, set up or downloaded until you say yes here.")
            }
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
