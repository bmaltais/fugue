pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../menu"
import "../canvas"
import "../kit"

// The "Get ready" sheet (spec 6.6, 8.9, 11.10; #379, #464): what the shown workflow needs (packs to install and trust, their
// environments, the missing models) as one list and one yes, then a row of progress each. The list, the checks and the work
// are App.workflows.readiness (C++, workflow/readiness_sheet.h, the same plan the CLI's `prepare` shows); this only shows it.
// Nothing happens until "Install".
//
// In the Drift kit (docs/canvas-look.md 20): a Moss KitPanel (Get ready and downloads), a KitHero with the one-line headline, the
// packs, environments and models as KitGroups of rows, where the models go as a row of its own, Install the one primary pill (a
// KitChip primary) and Cancel a raised chip. Keys: opening focuses Install (Cancel when there is nothing to install), Tab and
// Shift+Tab walk the rows (those with more to show), Change model folder and the buttons, Enter installs (a focused row or button
// takes its own Enter first), Escape cancels (stops a run), and the focus goes back to what had it before.
Popup {
    id: sheet
    objectName: "getReadySheet"
    property var sheetModel: App.workflows.readiness // not readonly: the picture tests give it a made-up plan
    readonly property var view: sheetModel.view
    readonly property string phase: sheetModel.phase
    readonly property CanvasLook look: App.theme.look
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    visible: sheetModel.open
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min((Overlay.overlay ? Overlay.overlay.width : 680) - 12 * look.unit, 680)
    exit: null
    Overlay.modal: Rectangle { color: App.theme.look.scrim }
    background: Item {
        Shadow { target: box; level: 3; look: sheet.look }
        Item { id: box; anchors.fill: parent }
    }
    // What had the focus before: the sheet gives it back when it goes.
    property Item opener: null
    onAboutToShow: opener = parent ? parent.Window.activeFocusItem : null
    onOpened: focusDefault()
    onClosed: if (opener) opener.forceActiveFocus()
    // The default control: Install while there is something to do, else the one answer that is left.
    readonly property Item defaultControl: [install, cancel, close].find(c => c.visible && c.enabled) ?? null
    function focusDefault() {
        if (defaultControl)
            defaultControl.forceActiveFocus()
    }
    // The plan can arrive (or the phase change) after the sheet opened, so the default control is another one by then: the focus
    // follows it unless the person put it on a row or the other button themselves, so Enter and Space work with no Tab first.
    onDefaultControlChanged: if (opened) Qt.callLater(refocus)
    function refocus() {
        if (!opened)
            return
        let item = parent ? parent.Window.activeFocusItem : null
        const lost = item === null || item === card || item === cancel || item === close
        while (item && item !== card)
            item = item.parent
        if (lost || !item)
            focusDefault()
    }
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

    contentItem: KitPanel {
        id: card
        objectName: "getReadyCard"
        look: sheet.look
        tone: "moss"
        clip: true
        implicitHeight: body.implicitHeight + 2
        focus: true
        // Tab walks the sheet's controls: it is also the window's "Add a node by search" key.
        Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab
        // The keys nothing under the focus took: Enter is the default (Install), Escape cancels.
        Keys.onEscapePressed: event => { if (sheet.phase === "finished") sheet.sheetModel.close(); else sheet.sheetModel.cancel(); event.accepted = true }
        Keys.onReturnPressed: event => { if (install.visible && install.enabled) sheet.sheetModel.start(); else if (sheet.phase === "finished") sheet.sheetModel.close(); event.accepted = true }
        Keys.onEnterPressed: event => { if (install.visible && install.enabled) sheet.sheetModel.start(); event.accepted = true }

        ColumnLayout {
            id: body
            anchors { fill: parent; margins: 1 }
            spacing: 0
            LayoutMirroring.enabled: sheet.rightToLeft
            LayoutMirroring.childrenInherit: true

            KitHero {
                id: hero
                objectName: "getReadyTitle"
                Layout.fillWidth: true
                look: sheet.look
                tone: card.tone
                title: qsTr("Get ready")
                subtitle: sheet.view.headline
            }

            // What it takes, in groups. It scrolls (up and down only) when it is taller than the window can spare; a row that is
            // too long is cut at its end, with the whole line as its tooltip, so there is no sideways bar.
            Flickable {
                id: list
                objectName: "getReadyList"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(rows.implicitHeight + 2 * sheet.look.unit, 0.45 * (Overlay.overlay ? Overlay.overlay.height : 800))
                clip: true
                contentWidth: width
                contentHeight: rows.implicitHeight + 2 * sheet.look.unit
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: FugueScrollBar { look: sheet.look }

                Column {
                    id: rows
                    x: 4 * sheet.look.unit
                    y: 0
                    width: list.width - 8 * sheet.look.unit - (list.ScrollBar.vertical as FugueScrollBar).breadth

                    KitSection {
                        look: sheet.look
                        visible: sheet.view.packs.length > 0
                        text: qsTr("Packs")
                        topPadding: 3 * sheet.look.unit
                    }
                    KitGroup {
                        look: sheet.look
                        visible: sheet.view.packs.length > 0
                        Repeater {
                            model: sheet.view.packs
                            GetReadyRow {
                                id: packRow
                                required property var modelData
                                readonly property string key: "pack:" + modelData.id
                                objectName: "getReadyPack-" + modelData.id
                                look: sheet.look
                                glyph: "boxes"
                                title: modelData.name + " " + modelData.version
                                subtitle: modelData.pending ? modelData.summary
                                          : (modelData.install ? qsTr("Install and trust") : qsTr("Trust"))
                                            + (modelData.source !== "" ? " · " + qsTr("from %1").arg(modelData.source) : "")
                                more: modelData.pending ? ""
                                      : qsTr("Runs as: %1").arg(modelData.runtime) + "\n" + qsTr("Declares: %1").arg(modelData.permissions)
                                        + (modelData.secrets !== "" ? "\n" + qsTr("Reads secrets: %1").arg(modelData.secrets) : "")
                                open: sheet.expanded[key] === true
                                stateText: sheet.stateText(modelData.state)
                                failed: modelData.state === "failed"
                                error: failed ? modelData.detail : ""
                                onToggled: sheet.toggle(key)
                            }
                        }
                    }

                    // A pack's environment is part of its row's work, so it shows the same state.
                    KitSection {
                        look: sheet.look
                        visible: envs.count > 0
                        text: qsTr("Environments")
                    }
                    KitGroup {
                        look: sheet.look
                        visible: envs.count > 0
                        Repeater {
                            id: envs
                            model: sheet.view.packs.filter(p => p.setup !== "")
                            GetReadyRow {
                                id: envRow
                                required property var modelData
                                readonly property string key: "env:" + modelData.id
                                objectName: "getReadyEnvironment-" + modelData.id
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
                    }

                    KitSection {
                        look: sheet.look
                        visible: sheet.view.models.length > 0
                        text: qsTr("Models")
                    }
                    KitGroup {
                        look: sheet.look
                        visible: sheet.view.models.length > 0
                        Repeater {
                            model: sheet.view.models
                            GetReadyRow {
                                id: modelRow
                                required property var modelData
                                readonly property string key: "model:" + modelData.name
                                objectName: "getReadyModel-" + modelData.name
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
                                useText: modelData.use && modelData.use.text ? modelData.use.text : ""
                                useBusy: App.workflows.hashingUses
                                onUseRequested: sheet.sheetModel.useOnDisk(modelData.hash)
                            }
                        }
                    }
                }
            }

            // Where models go (8.9), a group of its own that stays in view: the folder, the room in it against the download, and the way
            // to change it right beside them.
            Column {
                Layout.fillWidth: true
                Layout.leftMargin: 4 * sheet.look.unit
                Layout.rightMargin: 4 * sheet.look.unit
                visible: sheet.view.hasModels
            KitSection {
                look: sheet.look
                visible: sheet.view.hasModels
                text: qsTr("Destination")
            }
            KitGroup {
                look: sheet.look
                visible: sheet.view.hasModels
                KitRow {
                    id: destination
                    objectName: "getReadyDestination"
                    look: sheet.look
                    label: qsTr("Download models to")
                    hint: [sheet.view.destinationTitle, sheet.view.freeText,
                           sheet.view.sizeText !== "" ? qsTr("needs %1").arg(sheet.view.sizeText) : ""].filter(t => t !== "").join(" · ")
                    HoverHandler { id: destinationHover }
                    ToolTip.visible: destinationHover.hovered && sheet.view.destination !== ""
                    ToolTip.delay: sheet.look.tooltipDelay
                    ToolTip.text: sheet.view.destination
                    KitChip {
                        id: change
                        objectName: "getReadyChange"
                        look: sheet.look
                        visible: sheet.phase === "review"
                        text: qsTr("Change model folder…")
                        onTapped: destinationMenu.popup()
                    }
                }
            }
            }

            // What is wrong, what happened, the pack's question and the warning: quiet lines above the answers.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 6 * sheet.look.unit
                Layout.rightMargin: 6 * sheet.look.unit
                Layout.topMargin: sheet.look.unit
                spacing: 2 * sheet.look.unit
                Text {
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
                    Text {
                        required property string modelData
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: sheet.look.label
                        font.pixelSize: sheet.look.labelSize
                        text: modelData
                    }
                }
                // A pack fetched after the yes declares access the list could not show: asked once, for that row alone.
                ColumnLayout {
                    objectName: "getReadyQuestion"
                    visible: sheet.view.question.text !== undefined
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: sheet.look.text
                        font.bold: true
                        font.pixelSize: sheet.look.textSize
                        text: sheet.view.question.text ?? ""
                    }
                    RowLayout {
                        spacing: 2 * sheet.look.unit
                        KitChip {
                            objectName: "getReadySkip"
                            look: sheet.look
                            text: qsTr("Skip")
                            onTapped: sheet.sheetModel.answer(false)
                        }
                        KitChip {
                            objectName: "getReadyAllow"
                            look: sheet.look
                            text: qsTr("Allow")
                            onTapped: sheet.sheetModel.answer(true)
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: sheet.view.message !== ""
                    spacing: sheet.look.unit
                    Text {
                        id: readyMessage
                        objectName: "getReadyMessage"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: sheet.view.ok ? sheet.look.text : sheet.look.error
                        font.pixelSize: sheet.look.textSize
                        text: sheet.view.message + (sheet.phase !== "finished" || sheet.view.ok || sheet.view.retry.length === 0 ? "" : " " + qsTr("Not done: %1. Try again; it lists only what is left.").arg(sheet.view.retry.join(", ")))
                    }
                    KitCopyButton {
                        objectName: "getReadyMessageCopy"
                        visible: !sheet.view.ok // only a message that is an error
                        look: sheet.look
                        Layout.alignment: Qt.AlignTop
                        copyText: readyMessage.text
                    }
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
            }

            // The answers: Cancel or the safe one first, the one primary last at the end (D9).
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 4 * sheet.look.unit
                spacing: 2 * sheet.look.unit
                Item { Layout.fillWidth: true }
                KitChip {
                    id: cancel
                    objectName: "getReadyCancel"
                    look: sheet.look
                    bordered: true
                    visible: sheet.phase !== "finished"
                    text: sheet.phase === "running" ? qsTr("Stop") : qsTr("Cancel")
                    onTapped: sheet.sheetModel.cancel()
                }
                KitChip {
                    id: close
                    objectName: "getReadyClose"
                    look: sheet.look
                    bordered: true
                    visible: sheet.phase === "finished"
                    text: qsTr("Close")
                    onTapped: sheet.sheetModel.close()
                }
                KitChip {
                    objectName: "getReadyAgain"
                    look: sheet.look
                    bordered: true
                    visible: sheet.phase === "finished" && !sheet.view.ok
                    text: qsTr("Try again")
                    onTapped: sheet.sheetModel.show()
                }
                KitChip {
                    id: install
                    objectName: "getReadyAll"
                    look: sheet.look
                    primary: true
                    visible: sheet.phase === "review" && !sheet.view.empty
                    enabled: !sheet.view.lowSpace
                    text: qsTr("Install")
                    onTapped: sheet.sheetModel.start()
                }
            }
        }
    }
}
