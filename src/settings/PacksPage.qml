pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue

// The Packs page of the settings panel (spec 6.6-6.8, #25): the installed packs with what
// the load said about each, add by folder or pinned git address, update, trust and remove.
// Every change goes through a question the person answers here: the model installs, trusts,
// removes or runs a setup only after a yes, and shows exactly what it will do. Fugue enforces
// no sandbox, and the page says so.
ColumnLayout {
    id: page
    required property var host // the SettingsPanel
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property PacksModel model: host.model.packs
    spacing: 4 * u

    SettingsParts.Setting {
        title: qsTr("Packs")
        note: qsTr("Nodes come in packs. A trusted pack runs programs as you: Fugue enforces no sandbox, so trust only what you would run in a terminal.")
    }

    // The question: what will happen, and the person says yes or no.
    Rectangle {
        id: ask
        objectName: "packQuestion"
        visible: page.model.question.kind !== undefined
        readonly property var q: page.model.question
        Layout.fillWidth: true
        implicitHeight: askColumn.implicitHeight + 6 * page.u
        radius: page.look.smallCorner
        color: page.look.well
        border.width: 2
        border.color: page.look.accent
        ColumnLayout {
            id: askColumn
            x: 3 * page.u
            y: 3 * page.u
            width: ask.width - 6 * page.u
            spacing: page.u
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: page.look.text
                font.pixelSize: page.look.textSize
                font.weight: Font.DemiBold
                text: switch (ask.q.kind) {
                    case "install": return qsTr("Install %1 %2?").arg(ask.q.name).arg(ask.q.version)
                    case "update": return ask.q.oldVersion !== ""
                                          ? qsTr("Update %1 from %2 to %3?").arg(ask.q.name).arg(ask.q.oldVersion).arg(ask.q.version)
                                          : qsTr("Replace %1 with version %2?").arg(ask.q.name).arg(ask.q.version)
                    case "remove": return qsTr("Remove %1?").arg(ask.q.name)
                    case "trust": return qsTr("Trust %1 %2 and let its nodes run?").arg(ask.q.name).arg(ask.q.version)
                    case "setup": return qsTr("Run the setup of %1 %2?").arg(ask.q.name).arg(ask.q.version)
                    default: return ""
                }
            }
            Text {
                objectName: "packQuestionDetail"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: page.look.label
                font.pixelSize: page.look.labelSize
                text: ask.q.kind === "setup"
                      ? qsTr("%1\nIn %2, this command runs as you:\n%3").arg(ask.q.description).arg(ask.q.folder).arg(ask.q.command)
                      : qsTr("%1\nRuns as: %2\nDeclares: %3\nNo sandbox: a trusted pack runs programs as you.")
                            .arg(ask.q.source !== "" ? ask.q.id + "\n" + qsTr("From %1").arg(ask.q.source) : ask.q.id)
                            .arg(ask.q.runtime && ask.q.runtime.length ? ask.q.runtime.join(", ") : qsTr("no nodes"))
                            .arg(ask.q.permissions && ask.q.permissions.length ? ask.q.permissions.join(", ") : qsTr("nothing (network, files, GPU)"))
            }
            Row {
                spacing: page.u
                SettingsParts.SmallButton {
                    objectName: "packYes"
                    enabled: page.model.questionReady
                    highlighted: true
                    text: ask.q.kind === "setup" ? qsTr("Run it") : ask.q.kind === "remove" ? qsTr("Remove") : qsTr("Yes")
                    onClicked: page.model.answer(true)
                }
                SettingsParts.SmallButton {
                    objectName: "packNo"
                    enabled: page.model.questionReady
                    text: qsTr("No")
                    onClicked: page.model.answer(false)
                }
            }
        }
    }

    Text {
        visible: page.model.packs.length === 0
        text: qsTr("No packs installed.")
        color: page.look.label
        font.pixelSize: page.look.labelSize
    }

    Repeater {
        model: page.model.packs
        delegate: Rectangle {
            id: card
            required property var modelData
            required property int index
            objectName: "pack-" + modelData.id
            Layout.fillWidth: true
            implicitHeight: cardColumn.implicitHeight + 6 * page.u
            radius: page.look.smallCorner
            color: "transparent"
            border.width: 1
            border.color: page.look.hairline

            ColumnLayout {
                id: cardColumn
                x: 3 * page.u
                y: 3 * page.u
                width: card.width - 6 * page.u
                spacing: page.u
                RowLayout {
                    Layout.fillWidth: true
                    spacing: page.u
                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: (card.modelData.name !== "" ? card.modelData.name : card.modelData.id) + " " + card.modelData.version
                        color: page.look.text
                        font.pixelSize: page.look.textSize
                        font.weight: Font.DemiBold
                    }
                    Text {
                        objectName: "packState-" + card.modelData.id
                        text: !card.modelData.enabled ? qsTr("disabled")
                              : card.modelData.trusted ? qsTr("trusted") : qsTr("not trusted")
                        color: card.modelData.enabled && card.modelData.trusted ? page.look.label : page.look.error
                        font.pixelSize: page.look.captionSize
                    }
                    SettingsParts.SmallButton {
                        objectName: "packTrust-" + card.modelData.id
                        visible: card.modelData.enabled && !card.modelData.trusted
                        enabled: !page.model.busy
                        text: qsTr("Trust…")
                        onClicked: page.model.trust(card.modelData.id)
                    }
                    SettingsParts.SmallButton {
                        objectName: "packUpdate-" + card.modelData.id
                        visible: card.modelData.removable && card.modelData.source !== ""
                        enabled: !page.model.busy
                        text: qsTr("Update")
                        onClicked: page.model.update(card.modelData.id)
                    }
                    SettingsParts.SmallButton {
                        objectName: "packRemove-" + card.modelData.id
                        visible: card.modelData.removable
                        enabled: !page.model.busy
                        text: qsTr("Remove…")
                        onClicked: page.model.remove(card.modelData.id)
                    }
                }
                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: qsTr("%1 · %n nodes", "", card.modelData.nodes).arg(card.modelData.path)
                    color: page.look.label
                    font.pixelSize: page.look.captionSize
                }
                Repeater {
                    model: card.modelData.problems
                    delegate: Text {
                        required property string modelData
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: page.look.error
                        font.pixelSize: page.look.labelSize
                        text: modelData
                    }
                }
            }
        }
    }

    // The library's groups (#74): renamed here (folder and id follow) or removed after a yes.
    // Workflows keep their own copy of a group, so neither changes any of them.
    SettingsParts.Setting {
        visible: page.model.library.length > 0
        title: qsTr("Library")
        note: qsTr("Groups saved from workflows. A workflow keeps its own copy of each group it uses, so renaming or removing one here changes no workflow.")
    }
    Repeater {
        model: page.model.library
        delegate: RowLayout {
            id: entry
            required property var modelData
            property bool renaming: false
            property bool confirming: false
            objectName: "libraryGroup-" + modelData.id
            Layout.fillWidth: true
            spacing: page.u
            Text {
                visible: !entry.renaming
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: entry.confirming ? qsTr("Remove %1 from the library?").arg(entry.modelData.name)
                                       : entry.modelData.name + " " + entry.modelData.version
                color: page.look.text
                font.pixelSize: page.look.textSize
            }
            SettingsParts.Field {
                visible: entry.renaming
                objectName: "libraryName-" + entry.modelData.id
                value: entry.modelData.name
                onCommitted: text => {
                    entry.renaming = false
                    page.model.renameLibraryGroup(entry.modelData.id, text)
                }
            }
            SettingsParts.SmallButton {
                objectName: "libraryRename-" + entry.modelData.id
                enabled: !page.model.busy
                visible: !entry.renaming && !entry.confirming
                text: qsTr("Rename…")
                onClicked: entry.renaming = true
            }
            SettingsParts.SmallButton {
                objectName: "libraryRemove-" + entry.modelData.id
                enabled: !page.model.busy
                visible: !entry.renaming && !entry.confirming
                text: qsTr("Remove…")
                onClicked: entry.confirming = true
            }
            SettingsParts.SmallButton {
                objectName: "libraryRemoveNo-" + entry.modelData.id
                visible: entry.confirming
                text: qsTr("Keep")
                onClicked: entry.confirming = false
            }
            SettingsParts.SmallButton {
                objectName: "libraryRemoveYes-" + entry.modelData.id
                enabled: !page.model.busy
                visible: entry.confirming
                highlighted: true
                text: qsTr("Remove")
                onClicked: {
                    entry.confirming = false
                    page.model.removeLibraryGroup(entry.modelData.id)
                }
            }
        }
    }

    SettingsParts.Setting {
        title: qsTr("Add a pack")
        note: qsTr("A folder with a pack.json, or a git address pinned to a tag or a commit.")
        ColumnLayout {
            Layout.fillWidth: true
            spacing: page.u
            SettingsParts.Field {
                id: where
                objectName: "packLocation"
                placeholder: qsTr("Folder or git address")
                onCommitted: text => where.value = text
            }
            SettingsParts.Field {
                id: pin
                objectName: "packRef"
                placeholder: qsTr("Tag or commit (git only)")
                onCommitted: text => pin.value = text
            }
            SettingsParts.SmallButton {
                objectName: "packAdd"
                enabled: !page.model.busy
                text: qsTr("Add…")
                onClicked: {
                    where.commit()
                    pin.commit()
                    page.model.add(where.value, pin.value)
                }
            }
        }
    }
    SettingsParts.SmallButton {
        objectName: "packsUpdateAll"
        enabled: !page.model.busy
        text: qsTr("Update all")
        onClicked: page.model.update("")
    }

    Text {
        objectName: "packsBusy"
        visible: page.model.busy
        text: qsTr("Working…")
        color: page.look.label
        font.pixelSize: page.look.labelSize
    }
    SettingsParts.SmallButton {
        objectName: "packsCancel"
        visible: page.model.busy
        text: qsTr("Cancel")
        onClicked: page.model.cancel()
    }
    Text {
        objectName: "packsMessage"
        visible: text !== ""
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: page.look.text
        font.pixelSize: page.look.labelSize
        text: page.model.message
    }
    Text {
        objectName: "packsFailure"
        visible: text !== ""
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: page.look.error
        font.pixelSize: page.look.labelSize
        text: page.model.failure
    }
    Text {
        visible: text !== ""
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: page.look.label
        font.family: "monospace"
        font.pixelSize: page.look.captionSize
        text: page.model.log.slice(-12).join("\n")
    }
}
