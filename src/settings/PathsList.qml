pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue
import "../kit"

// The body of an open row of the Folders page: a list of folders (model and pack search paths), each with a remove button, then a
// field and Choose... to add one. Its own file, so it can be Bound. With `kinds`, each model folder also says what it holds (#86):
// found by its files (and what was found), ComfyUI's layout, or one kind of model. The folders of the Default project this project
// reads come after its own, read-only, with the origin tag; their remove button answers with the refusal (spec 1.4.2).
Column {
    id: paths
    required property var host // the SettingsPanel: its model, apply() and pickFolder()
    property string key
    property string note
    property var value: []
    property var inherited: []
    property bool kinds: false
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property real padLeft: look.params.rowPadLeft * u
    readonly property real padRight: look.params.rowPadRight * u
    width: parent.width

    Item {
        visible: paths.note !== ""
        width: parent.width
        height: noteText.implicitHeight + 3 * paths.u
        Text {
            id: noteText
            x: paths.padLeft
            y: 1.5 * paths.u
            width: parent.width - paths.padLeft - paths.padRight
            text: paths.note
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: paths.look.label
            font.pixelSize: paths.look.labelSize
        }
    }

    Repeater {
        model: paths.value
        delegate: Column {
            id: pathRow
            required property string modelData
            required property int index
            width: paths.width
            KitSeparator { look: paths.look }
            Item {
                width: parent.width
                height: own.implicitHeight + 3 * paths.u
                ColumnLayout {
                    id: own
                    x: paths.padLeft
                    y: 1.5 * paths.u
                    width: parent.width - paths.padLeft - paths.padRight
                    spacing: paths.u
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 2 * paths.u
                        Text {
                            Layout.fillWidth: true
                            text: pathRow.modelData
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            color: paths.look.text
                            font.pixelSize: paths.look.textSize
                        }
                        KitChip {
                            objectName: paths.key + "-remove-" + pathRow.index
                            look: paths.look
                            glyph: "x"
                            round: true
                            tip: qsTr("Remove")
                            Accessible.name: qsTr("Remove %1").arg(pathRow.modelData)
                            onTapped: paths.host.apply(paths.host.model.removePath(paths.key, pathRow.index))
                        }
                    }
                    // What it holds, under its path: the choice, and what its files say when they decide.
                    Choices {
                        visible: paths.kinds
                        Layout.fillWidth: true
                        name: paths.key + "-holds-" + pathRow.index
                        current: paths.host.model.modelFolderKindsSet[pathRow.modelData] ?? ""
                        options: paths.host.model.modelFolderKinds()
                        onPicked: value => paths.host.apply(paths.host.model.setModelFolderKind(pathRow.modelData, value))
                    }
                    Text {
                        objectName: paths.key + "-found-" + pathRow.index
                        visible: paths.kinds && (paths.host.model.modelFolderKindsSet[pathRow.modelData] ?? "") === ""
                        Layout.fillWidth: true
                        text: paths.host.model.modelFolderFound(pathRow.modelData)
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: paths.look.label
                        font.pixelSize: paths.look.labelSize
                    }
                }
            }
        }
    }

    Repeater {
        model: paths.inherited
        delegate: Column {
            id: inheritedRow
            required property string modelData
            required property int index
            objectName: paths.key + "-inherited-" + index
            width: paths.width
            KitSeparator { look: paths.look }
            Item {
                width: parent.width
                height: theirs.implicitHeight + 3 * paths.u
                ColumnLayout {
                    id: theirs
                    x: paths.padLeft
                    y: 1.5 * paths.u
                    width: parent.width - paths.padLeft - paths.padRight
                    spacing: paths.u
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 2 * paths.u
                        Text {
                            Layout.fillWidth: true
                            text: inheritedRow.modelData
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            color: paths.look.label
                            font.pixelSize: paths.look.textSize
                        }
                        KitBadge {
                            objectName: paths.key + "-inherited-tag-" + inheritedRow.index
                            look: paths.look
                            text: qsTr("Inherited from Default")
                        }
                        KitChip {
                            objectName: paths.key + "-inherited-remove-" + inheritedRow.index
                            look: paths.look
                            glyph: "x"
                            round: true
                            tip: qsTr("Remove")
                            Accessible.name: qsTr("Remove %1").arg(inheritedRow.modelData)
                            onTapped: paths.host.apply(paths.host.model.removeInheritedPath(paths.key, inheritedRow.modelData))
                        }
                    }
                    // What the Default says it holds, when it says (read-only: it is the Default's to change).
                    Text {
                        readonly property string kind: paths.host.model.modelFolderKindsSet[inheritedRow.modelData] ?? ""
                        visible: paths.kinds && kind !== ""
                        Layout.fillWidth: true
                        text: qsTr("Holds: %1").arg(paths.host.model.modelFolderKinds().find(k => k.value === kind)?.text ?? kind)
                        textFormat: Text.PlainText
                        color: paths.look.label
                        font.pixelSize: paths.look.labelSize
                    }
                }
            }
        }
    }

    KitRow {
        look: paths.look
        label: qsTr("Add a folder")
        hint: qsTr("Type its path and press Enter")
        SettingsParts.Field {
            implicitWidth: Math.min(60 * paths.u, paths.width - 16 * paths.u) // never wider than the tray, narrow or wide
            id: addField
            objectName: paths.key + "-add"
            label: qsTr("Add a folder")
            placeholder: qsTr("Path of the folder")
            onCommitted: text => {
                if (text.trim() !== "" && paths.host.apply(paths.host.model.addPath(paths.key, text)))
                    addField.input.text = ""
            }
        }
    }
    KitRow {
        look: paths.look
        label: qsTr("Choose a folder")
        hint: qsTr("Opens your file chooser")
        KitChip {
            objectName: paths.key + "-choose"
            look: paths.look
            text: qsTr("Choose…")
            onTapped: paths.host.pickFolder(url => paths.host.apply(paths.host.model.addPath(paths.key, url)))
        }
    }
}
