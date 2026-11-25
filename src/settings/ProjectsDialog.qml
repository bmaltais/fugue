pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../menu"
import "../canvas"

// The Projects dialog (spec 1.4.2, #204 step 2), opened by file.projects (File > Projects…) or the project
// chip: the projects you know with the current one marked, and New…, Open folder… and Project settings….
// Switching starts a new Fugue window in that project (project_model.h); this one closes, or, with unsaved
// work, stays and offers to open the other project in a new window. Removing a project from the list never
// touches its folder. One popup with four views (list, new, settings, blocked); styled as the settings panel.
Popup {
    id: dialog
    objectName: "projectsDialog"
    required property ProjectModel model

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    // "list", "new", "settings" or "blocked".
    property string view: "list"
    // Why the last call was refused, until the next one.
    property string error: ""
    // The row asking "Remove from the list?", by root.
    property string removing: ""
    // The switch that waits for an answer (the window has unsaved work).
    property string blockedRoot: ""
    property string blockedName: ""
    // The words ticked in the New and the settings views.
    property var newInherits: []
    property var settingsInherits: []

    function reset() {
        view = "list"
        error = ""
        removing = ""
        model.refresh()
    }
    // Runs a call that returns a refusal, or "" when done.
    function apply(result) { error = result; return result === "" }

    function toggled(list, id, on) {
        const out = list.filter(word => word !== id)
        if (on)
            out.push(id)
        return out
    }

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(680, (parent ? parent.width : 680) - 12 * u)
    height: Math.min(content.implicitHeight, (parent ? parent.height : 640) - 12 * u)
    Overlay.modal: Rectangle { color: dialog.look.scrim }
    background: PopupSurface { look: dialog.look }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; to: 0; duration: 90; easing.type: Easing.OutCubic }
    }
    onAboutToShow: reset()
    // The first useful control of the list takes the keys; Tab walks New, Open folder and Project settings, Escape closes.
    onOpened: if (dialog.view === "list") newButton.forceActiveFocus()

    Connections {
        target: dialog.model
        function onSwitchBlocked(root, name) {
            dialog.blockedRoot = root
            dialog.blockedName = name
            dialog.view = "blocked"
        }
    }

    Component {
        id: folderDialog
        FilePicker {
            id: d
            objectName: "projectFolderDialog"
            fileMode: PortalFileDialog.Directory
            property var done: null
            onAccepted: { if (d.done) d.done(d.selectedFolder); d.destroy() }
            onRejected: d.destroy()
        }
    }
    function pickFolder(callback) {
        const picker = folderDialog.createObject(dialog, {done: callback}) as FilePicker
        picker.open()
    }

    contentItem: Flickable {
        id: scroller
        clip: true
        contentWidth: width
        contentHeight: content.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: FugueScrollBar { look: dialog.look }
        LayoutMirroring.enabled: dialog.rightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            id: content
            x: 6 * dialog.u
            width: scroller.width - 12 * dialog.u - (scroller.ScrollBar.vertical as FugueScrollBar).inset
            spacing: 4 * dialog.u

            Item { implicitHeight: 2 * dialog.u }

            // --- the list ---
            ColumnLayout {
                visible: dialog.view === "list"
                Layout.fillWidth: true
                spacing: 3 * dialog.u
                Text {
                    text: qsTr("Projects")
                    color: dialog.look.text
                    font.pixelSize: dialog.look.overlayTitleSize
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("A project keeps its own settings, packs, models, outputs and keys apart from the others. Switching opens a new Fugue window in the project you pick.")
                    color: dialog.look.label
                    font.pixelSize: dialog.look.labelSize
                }
                Text {
                    objectName: "projectsRegistryProblem"
                    visible: text !== ""
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: dialog.model.registryProblem
                    color: dialog.look.error
                    font.pixelSize: dialog.look.labelSize
                }
                Repeater {
                    model: dialog.model.projects
                    delegate: Rectangle {
                        id: row
                        required property var modelData
                        required property int index
                        readonly property bool usable: modelData.state === "ok"
                        // The question is on this row (the Default's root is empty, as `removing` is when none asks).
                        readonly property bool asking: dialog.removing !== "" && dialog.removing === modelData.root
                        objectName: "project-" + index
                        Layout.fillWidth: true
                        implicitHeight: rowColumn.implicitHeight + 6 * dialog.u
                        radius: Math.min(dialog.look.params.cornerGroup * dialog.u, 0.3 * height)
                        color: modelData.current ? dialog.look.selectedRow : "transparent"
                        border.width: 1
                        border.color: dialog.look.hairline
                        ColumnLayout {
                            id: rowColumn
                            x: 3 * dialog.u
                            y: 3 * dialog.u
                            width: row.width - 6 * dialog.u
                            spacing: dialog.u
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 2 * dialog.u
                                Text {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: row.modelData.name
                                    color: row.usable ? dialog.look.text : dialog.look.label
                                    font.pixelSize: dialog.look.textSize
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    objectName: "projectCurrent-" + row.index
                                    visible: row.modelData.current
                                    text: qsTr("Current")
                                    color: dialog.look.accent
                                    font.pixelSize: dialog.look.captionSize
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    objectName: "projectState-" + row.index
                                    visible: row.modelData.state !== "ok"
                                    text: row.modelData.state === "missing" ? qsTr("Missing") : qsTr("Invalid") // status words capitalized, as Current
                                    color: dialog.look.error
                                    font.pixelSize: dialog.look.captionSize
                                    font.weight: Font.DemiBold
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                                text: row.modelData.path
                                color: dialog.look.label
                                font.pixelSize: dialog.look.captionSize
                            }
                            Text {
                                visible: row.modelData.stateText !== ""
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: row.modelData.stateText
                                color: row.modelData.state === "ok" ? dialog.look.label : dialog.look.error
                                font.pixelSize: dialog.look.labelSize
                            }
                            Text {
                                visible: !row.modelData.isDefault && row.usable
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: row.modelData.inherits.length > 0
                                      ? qsTr("Takes from the Default project: %1").arg(row.modelData.inherits.join(", "))
                                      : qsTr("Takes nothing from the Default project")
                                color: dialog.look.label
                                font.pixelSize: dialog.look.captionSize
                            }
                            // A question, not a button: only the list is touched, never the folder.
                            Text {
                                objectName: "projectRemoveQuestion-" + row.index
                                visible: row.asking
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: qsTr("Remove %1 from the list? Its folder and files are kept; Open folder… adds it back.").arg(row.modelData.name)
                                color: dialog.look.text
                                font.pixelSize: dialog.look.labelSize
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: dialog.u
                                SettingsParts.SmallButton {
                                    objectName: "projectSwitch-" + row.index
                                    visible: !row.modelData.current && !row.asking
                                    enabled: row.usable
                                    text: qsTr("Switch")
                                    Accessible.description: qsTr("Opens a new window in %1 and closes this one").arg(row.modelData.name)
                                    onClicked: dialog.apply(dialog.model.switchTo(row.modelData.root))
                                }
                                SettingsParts.SmallButton {
                                    objectName: "projectNewWindow-" + row.index
                                    visible: !row.modelData.current && !row.asking
                                    enabled: row.usable
                                    text: qsTr("Open in new window")
                                    onClicked: dialog.apply(dialog.model.openInNewWindow(row.modelData.root))
                                }
                                SettingsParts.SmallButton {
                                    objectName: "projectRemove-" + row.index
                                    visible: !row.modelData.isDefault && !row.asking && row.modelData.registered
                                    danger: true
                                    text: qsTr("Remove from list…")
                                    onClicked: dialog.removing = row.modelData.root
                                }
                                // The safe answer is the highlighted one, the removal last and apart, in the danger tint
                                // (docs/ui-ergonomics.md: Destructive; the cohesion audit's D8).
                                SettingsParts.SmallButton {
                                    objectName: "projectRemoveNo-" + row.index
                                    visible: row.asking
                                    highlighted: true
                                    text: qsTr("Keep")
                                    onClicked: dialog.removing = ""
                                }
                                Item { width: dialog.u; height: 1; visible: row.asking }
                                SettingsParts.SmallButton {
                                    objectName: "projectRemoveYes-" + row.index
                                    visible: row.asking
                                    danger: true
                                    text: qsTr("Remove from list")
                                    onClicked: {
                                        dialog.removing = ""
                                        dialog.apply(dialog.model.remove(row.modelData.root))
                                    }
                                }
                            }
                        }
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: dialog.u
                    SettingsParts.SmallButton {
                        id: newButton
                        objectName: "projectNew"
                        text: qsTr("New…")
                        onClicked: {
                            dialog.error = ""
                            dialog.newInherits = dialog.model.defaultInherits
                            newName.input.text = ""
                            newFolder.input.text = ""
                            dialog.view = "new"
                            newName.input.forceActiveFocus()
                        }
                    }
                    SettingsParts.SmallButton {
                        objectName: "projectOpenFolder"
                        text: qsTr("Open folder…")
                        onClicked: dialog.pickFolder(url => dialog.apply(dialog.model.addFolder(url)))
                    }
                    SettingsParts.SmallButton {
                        objectName: "projectSettings"
                        text: qsTr("Project settings…")
                        onClicked: {
                            dialog.error = ""
                            dialog.settingsInherits = dialog.model.currentInherits
                            settingsName.input.text = dialog.model.currentName
                            dialog.view = "settings"
                        }
                    }
                }
            }

            // --- New ---
            ColumnLayout {
                visible: dialog.view === "new"
                Layout.fillWidth: true
                spacing: 3 * dialog.u
                Text {
                    text: qsTr("New project")
                    color: dialog.look.text
                    font.pixelSize: dialog.look.overlayTitleSize
                    font.weight: Font.DemiBold
                }
                SettingsParts.Setting {
                    title: qsTr("Name")
                    note: qsTr("What the window title and the project chip show. No slash or dot.")
                    SettingsParts.Field {
                        id: newName
                        objectName: "newProjectName"
                        placeholder: qsTr("Name")
                    }
                }
                SettingsParts.Setting {
                    title: qsTr("Folder")
                    note: newFolder.input.text === "" ? qsTr("Left empty, it goes to %1.").arg(dialog.model.defaultLocation(newName.input.text))
                                                      : qsTr("The folder becomes the project's root; it is made if it is not there.")
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: dialog.u
                        SettingsParts.Field {
                            id: newFolder
                            objectName: "newProjectFolder"
                            placeholder: qsTr("Optional: another folder, such as a client's")
                        }
                        SettingsParts.SmallButton {
                            objectName: "newProjectChoose"
                            text: qsTr("Choose…")
                            onClicked: dialog.pickFolder(url => newFolder.input.text = decodeURIComponent(url.toString().replace("file://", "")))
                        }
                    }
                }
                SettingsParts.Setting {
                    title: qsTr("Take from the Default project")
                    note: qsTr("Read-only here: the project never changes the Default's files. Providers and keys, recent files, results, outputs and workflows are always the project's own.")
                    Repeater {
                        model: dialog.model.inheritChoices
                        delegate: ColumnLayout {
                            id: choice
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 0
                            SettingsParts.Toggle {
                                objectName: "newInherits-" + choice.modelData.id
                                text: choice.modelData.title
                                on: dialog.newInherits.indexOf(choice.modelData.id) >= 0
                                onToggled: value => dialog.newInherits = dialog.toggled(dialog.newInherits, choice.modelData.id, value)
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: 26 + 2 * dialog.u
                                wrapMode: Text.Wrap
                                text: choice.modelData.description
                                color: dialog.look.label
                                font.pixelSize: dialog.look.captionSize
                            }
                        }
                    }
                }
                // Back or Cancel first, the primary last at the right (docs/ui-ergonomics.md, Cohesion checklist: Order).
                Row {
                    Layout.alignment: Qt.AlignRight
                    spacing: dialog.u
                    SettingsParts.SmallButton {
                        objectName: "newProjectBack"
                        text: qsTr("Back")
                        onClicked: { dialog.error = ""; dialog.view = "list" }
                    }
                    SettingsParts.SmallButton {
                        objectName: "newProjectCreate"
                        text: qsTr("Create")
                        onClicked: {
                            if (dialog.apply(dialog.model.create(newName.input.text, newFolder.input.text, dialog.newInherits).error))
                                dialog.view = "list"
                        }
                    }
                    SettingsParts.SmallButton {
                        objectName: "newProjectCreateOpen"
                        highlighted: true
                        text: qsTr("Create and open")
                        onClicked: {
                            const made = dialog.model.create(newName.input.text, newFolder.input.text, dialog.newInherits)
                            if (dialog.apply(made.error))
                                dialog.apply(dialog.model.switchTo(made.root))
                        }
                    }
                }
            }

            // --- Project settings ---
            ColumnLayout {
                visible: dialog.view === "settings"
                Layout.fillWidth: true
                spacing: 3 * dialog.u
                Text {
                    text: qsTr("Project settings")
                    color: dialog.look.text
                    font.pixelSize: dialog.look.overlayTitleSize
                    font.weight: Font.DemiBold
                }
                Text {
                    objectName: "defaultProjectNote"
                    visible: dialog.model.currentIsDefault
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("You are in the Default project: the ordinary Fugue, with your own settings, models and packs. It has no name to change and takes nothing from another project; other projects can take from it. Nothing here can remove or reset it.")
                    color: dialog.look.label
                    font.pixelSize: dialog.look.labelSize
                }
                Text {
                    visible: dialog.model.currentIsDefault
                    text: dialog.model.indicator
                    color: dialog.look.text
                    font.pixelSize: dialog.look.textSize
                    font.weight: Font.DemiBold
                }
                SettingsParts.Setting {
                    visible: !dialog.model.currentIsDefault
                    title: qsTr("Name")
                    note: qsTr("The folder stays where it is: %1").arg(dialog.model.currentRoot)
                    SettingsParts.Field {
                        id: settingsName
                        objectName: "projectName"
                        placeholder: qsTr("Name")
                    }
                }
                SettingsParts.Setting {
                    visible: !dialog.model.currentIsDefault
                    title: qsTr("Take from the Default project")
                    note: qsTr("Read-only here. Turning one off hides the Default's items again; nothing is deleted anywhere.")
                    Repeater {
                        model: dialog.model.inheritChoices
                        delegate: ColumnLayout {
                            id: setChoice
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 0
                            SettingsParts.Toggle {
                                objectName: "projectInherits-" + setChoice.modelData.id
                                text: setChoice.modelData.title
                                on: dialog.settingsInherits.indexOf(setChoice.modelData.id) >= 0
                                onToggled: value => dialog.settingsInherits = dialog.toggled(dialog.settingsInherits, setChoice.modelData.id, value)
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: 26 + 2 * dialog.u
                                wrapMode: Text.Wrap
                                text: setChoice.modelData.description
                                color: dialog.look.label
                                font.pixelSize: dialog.look.captionSize
                            }
                        }
                    }
                }
                // Back or Cancel first, the primary last at the right (docs/ui-ergonomics.md, Cohesion checklist: Order).
                Row {
                    Layout.alignment: Qt.AlignRight
                    spacing: dialog.u
                    SettingsParts.SmallButton {
                        objectName: "projectSettingsBack"
                        highlighted: dialog.model.currentIsDefault
                        text: dialog.model.currentIsDefault ? qsTr("Close") : qsTr("Back")
                        onClicked: { dialog.error = ""; dialog.view = "list" }
                    }
                    SettingsParts.SmallButton {
                        objectName: "projectSettingsSave"
                        visible: !dialog.model.currentIsDefault
                        highlighted: true
                        text: qsTr("Save")
                        onClicked: {
                            if (dialog.apply(dialog.model.setCurrent(settingsName.input.text, dialog.settingsInherits))) {
                                if (App.settings)
                                    App.settings.reload()
                                dialog.view = "list"
                            }
                        }
                    }
                }
            }

            // --- unsaved work ---
            ColumnLayout {
                visible: dialog.view === "blocked"
                Layout.fillWidth: true
                spacing: 3 * dialog.u
                Text {
                    text: qsTr("Unsaved work")
                    color: dialog.look.text
                    font.pixelSize: dialog.look.overlayTitleSize
                    font.weight: Font.DemiBold
                }
                Text {
                    objectName: "projectBlockedText"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("This window has workflows with unsaved changes, so it stays open. Open %1 in a new window and keep working here, or save and then switch.").arg(dialog.blockedName)
                    color: dialog.look.text
                    font.pixelSize: dialog.look.textSize
                }
                // Back or Cancel first, the primary last at the right (docs/ui-ergonomics.md, Cohesion checklist: Order).
                Row {
                    Layout.alignment: Qt.AlignRight
                    spacing: dialog.u
                    SettingsParts.SmallButton {
                        objectName: "projectBlockedCancel"
                        text: qsTr("Cancel")
                        onClicked: { dialog.error = ""; dialog.view = "list" }
                    }
                    SettingsParts.SmallButton {
                        objectName: "projectBlockedNewWindow"
                        highlighted: true
                        text: qsTr("Open in new window")
                        onClicked: {
                            if (dialog.apply(dialog.model.openInNewWindow(dialog.blockedRoot)))
                                dialog.view = "list"
                        }
                    }
                }
            }

            Text {
                objectName: "projectsError"
                visible: dialog.error !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: dialog.error
                color: dialog.look.error
                font.pixelSize: dialog.look.labelSize
            }
            Text {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Esc to close")
                color: dialog.look.label
                font.pixelSize: dialog.look.captionSize
            }
            Item { implicitHeight: 2 * dialog.u }
        }
    }
}
