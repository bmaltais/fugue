pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue

// A list of folders (model and pack search paths): each with a remove button, then a
// field and Choose… to add one. Its own file, not an inline component of SettingsParts, so
// it can be Bound. With `kinds`, each model folder also says what it holds (#86): found by its
// files (and what was found), ComfyUI's layout, or one kind of model.
SettingsParts.Setting {
    id: paths
    required property var host
    property string key
    property var value: []
    property bool kinds: false
    ColumnLayout {
        Layout.fillWidth: true
        spacing: App.theme.look.unit
        Repeater {
            model: paths.value
            delegate: ColumnLayout {
                id: pathRow
                required property string modelData
                required property int index
                Layout.fillWidth: true
                spacing: App.theme.look.unit
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: pathRow.modelData
                        elide: Text.ElideMiddle
                        color: App.theme.look.text
                        font.pixelSize: App.theme.look.textSize
                    }
                    SettingsParts.SmallButton {
                        objectName: paths.key + "-remove-" + pathRow.index
                        glyph: "x"
                        Accessible.name: qsTr("Remove %1").arg(pathRow.modelData)
                        onClicked: paths.host.apply(paths.host.model.removePath(paths.key, pathRow.index))
                    }
                }
                // What it holds, under its path: the choice, and what its files say when
                // they decide.
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
                    color: App.theme.look.label
                    font.pixelSize: App.theme.look.labelSize
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: App.theme.look.unit
            SettingsParts.Field {
                id: addField
                objectName: paths.key + "-add"
                placeholder: qsTr("Add a folder: type its path and press Enter")
                onCommitted: text => {
                    if (text.trim() !== "" && paths.host.apply(paths.host.model.addPath(paths.key, text)))
                        addField.input.text = ""
                }
            }
            SettingsParts.SmallButton {
                text: qsTr("Choose…")
                onClicked: paths.host.pickFolder(url => paths.host.apply(paths.host.model.addPath(paths.key, url)))
            }
        }
    }
}
