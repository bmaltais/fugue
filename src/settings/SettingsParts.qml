import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// The settings panel's building blocks (#51), as inline components: SettingsParts.Setting
// and the rest (not Bound: another file cannot make a bound inline component). They draw with theme.look (docs/canvas-look.md) and only report what the
// user did; the panel and its pages call the model.
QtObject {
    // A titled setting: the name and what it does on the left, its control under them.
    component Setting: ColumnLayout {
        id: setting
        property string title
        property string note
        Layout.fillWidth: true
        spacing: App.theme.look.unit
        Text {
            text: setting.title
            color: App.theme.look.text
            font.pixelSize: App.theme.look.textSize
            font.weight: Font.DemiBold
        }
        Text {
            visible: text !== ""
            Layout.fillWidth: true
            text: setting.note
            wrapMode: Text.Wrap
            color: App.theme.look.label
            font.pixelSize: App.theme.look.labelSize
        }
    }

    // A small button: a hairline edge on `well`, its label or glyph inside.
    component SmallButton: Rectangle {
        id: button
        property string text
        property string glyph
        property bool highlighted: false
        signal clicked()
        implicitWidth: glyph !== "" && text === "" ? 7 * App.theme.look.unit : label.implicitWidth + 6 * App.theme.look.unit
        implicitHeight: 7 * App.theme.look.unit
        radius: App.theme.look.smallCorner
        color: highlighted ? App.theme.look.accent : hover.hovered ? App.theme.look.selectedRow : App.theme.look.well
        border.width: 1
        border.color: activeFocus ? App.theme.look.accent : App.theme.look.hairlineStrong
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: text
        Keys.onSpacePressed: clicked()
        Keys.onReturnPressed: clicked()
        HoverHandler { id: hover }
        TapHandler { onTapped: button.clicked() }
        Glyph {
            visible: button.glyph !== ""
            anchors.centerIn: parent
            name: button.glyph
            look: App.theme.look
            color: App.theme.look.text
            font.pixelSize: 12
        }
        Text {
            id: label
            visible: button.text !== ""
            anchors.centerIn: parent
            text: button.text
            color: button.highlighted ? App.theme.look.accentForeground : App.theme.look.text
            font.pixelSize: App.theme.look.labelSize
            font.weight: Font.Medium
        }
    }

    // A row of choices, one of them current (the segmented control of canvas-look 4.4).
    component Choices: Flow {
        id: choices
        property var options: [] // {value, text}
        property var current
        property string name // objectName prefix: <name>-<value>
        signal picked(var value)
        spacing: App.theme.look.unit
        Repeater {
            model: choices.options
            delegate: SmallButton {
                required property var modelData
                objectName: choices.name + "-" + modelData.value
                text: modelData.text
                highlighted: modelData.value === choices.current
                Accessible.checked: highlighted
                onClicked: choices.picked(modelData.value)
            }
        }
    }

    // A one-line text field in a well. It commits on Enter or when focus leaves; the model
    // decides, and a refused text goes back to the value.
    component Field: Rectangle {
        id: field
        property string value
        property string placeholder
        property bool secret: false
        property alias input: input
        signal committed(string text)
        Layout.fillWidth: true
        implicitWidth: 60 * App.theme.look.unit
        implicitHeight: 8 * App.theme.look.unit
        radius: App.theme.look.smallCorner
        color: App.theme.look.well
        border.width: 1
        border.color: input.activeFocus ? App.theme.look.accent : App.theme.look.hairlineStrong
        function commit() {
            if (input.text !== field.value)
                field.committed(input.text)
            input.text = Qt.binding(() => field.secret ? "" : field.value)
        }
        TextInput {
            id: input
            anchors { left: parent.left; right: parent.right; leftMargin: 2 * App.theme.look.unit; rightMargin: 2 * App.theme.look.unit
                      verticalCenter: parent.verticalCenter }
            clip: true
            selectByMouse: true
            text: field.secret ? "" : field.value
            echoMode: field.secret ? TextInput.Password : TextInput.Normal
            color: App.theme.look.text
            selectionColor: App.theme.look.selectedRow
            selectedTextColor: App.theme.look.text
            font.pixelSize: App.theme.look.textSize
            Accessible.role: Accessible.EditableText
            Accessible.name: field.placeholder
            onAccepted: field.commit()
            onActiveFocusChanged: if (!activeFocus && text !== (field.secret ? "" : field.value)) field.commit()
            Keys.onEscapePressed: event => {
                if (input.text !== (field.secret ? "" : field.value)) {
                    input.text = Qt.binding(() => field.secret ? "" : field.value)
                    event.accepted = true
                } else {
                    event.accepted = false
                }
            }
            Text {
                visible: input.text === ""
                anchors.verticalCenter: parent.verticalCenter
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: field.placeholder
                color: App.theme.look.label
                font: input.font
            }
        }
    }

    // A folder setting: the folder in effect, Choose… (the portal), and Default when chosen.
    component FolderSetting: Setting {
        id: folder
        required property var host // the panel: its model and pickers
        property string key
        property string value
        property bool isDefault
        RowLayout {
            Layout.fillWidth: true
            spacing: App.theme.look.unit
            Field {
                objectName: folder.key
                value: folder.value
                placeholder: folder.title
                onCommitted: text => folder.host.apply(folder.host.model.set(folder.key, text))
            }
            SmallButton {
                text: qsTr("Choose…")
                onClicked: folder.host.pickFolder(url => folder.host.apply(folder.host.model.set(folder.key, url)))
            }
            SmallButton {
                visible: !folder.isDefault
                objectName: folder.key + "-default"
                text: qsTr("Default")
                onClicked: folder.host.apply(folder.host.model.set(folder.key, ""))
            }
        }
    }

    // A list of folders (model and pack search paths): each with a remove button, then a
    // field and Choose… to add one.
    component PathsSetting: Setting {
        id: paths
        required property var host
        property string key
        property var value: []
        ColumnLayout {
            Layout.fillWidth: true
            spacing: App.theme.look.unit
            Repeater {
                model: paths.value
                delegate: RowLayout {
                    id: pathRow
                    required property string modelData
                    required property int index
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: pathRow.modelData
                        elide: Text.ElideMiddle
                        color: App.theme.look.text
                        font.pixelSize: App.theme.look.textSize
                    }
                    SmallButton {
                        objectName: paths.key + "-remove-" + pathRow.index
                        glyph: "x"
                        Accessible.name: qsTr("Remove %1").arg(pathRow.modelData)
                        onClicked: paths.host.apply(paths.host.model.removePath(paths.key, pathRow.index))
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: App.theme.look.unit
                Field {
                    id: addField
                    objectName: paths.key + "-add"
                    placeholder: qsTr("Add a folder: type its path and press Enter")
                    onCommitted: text => {
                        if (text.trim() !== "" && paths.host.apply(paths.host.model.addPath(paths.key, text)))
                            addField.input.text = ""
                    }
                }
                SmallButton {
                    text: qsTr("Choose…")
                    onClicked: paths.host.pickFolder(url => paths.host.apply(paths.host.model.addPath(paths.key, url)))
                }
            }
        }
    }

}
