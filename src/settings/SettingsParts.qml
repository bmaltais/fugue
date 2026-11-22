import QtQuick
import QtQuick.Layouts
import Fugue
import "../canvas"

// The settings panel's building blocks (#51), as inline components: SettingsParts.Setting
// and the rest (not Bound: another file cannot make a bound inline component). They draw with theme.look (docs/canvas-look.md) and only report what the
// user did; the panel and its pages call the model.
QtObject {
    // A titled setting: the name and what it does on the left, its control under them.
    // With `key` and `origin` ("inherited" or "override", from SettingsModel.origins) it also says where the value
    // comes from (spec 1.4.2): "Inherited from Default", or, for the project's own value over the Default's, a
    // button that gives it up (useDefaultRequested).
    component Setting: ColumnLayout {
        id: setting
        property string title
        property string note
        property string key
        property string origin
        signal useDefaultRequested()
        Layout.fillWidth: true
        spacing: App.theme.look.unit
        RowLayout {
            Layout.fillWidth: true
            spacing: 2 * App.theme.look.unit
            Text {
                text: setting.title
                color: App.theme.look.text
                font.pixelSize: App.theme.look.textSize
                font.weight: Font.DemiBold
            }
            Text {
                objectName: "origin-" + setting.key
                visible: setting.origin === "inherited"
                text: qsTr("Inherited from Default")
                color: App.theme.look.accent
                font.pixelSize: App.theme.look.captionSize
            }
            SmallButton {
                objectName: "useDefault-" + setting.key
                visible: setting.origin === "override"
                text: qsTr("Use Default's value")
                onClicked: setting.useDefaultRequested()
            }
            Item { Layout.fillWidth: true }
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

    // An on/off switch with its label, the same track and knob as a toggle widget (canvas-look
    // 4.4) and Providers' "Use" switch. The whole row is the press target, 7u = 28px tall.
    component Toggle: Rectangle {
        id: toggle
        property bool on: false
        property string text
        signal toggled(bool value)
        implicitWidth: 22 + App.theme.look.unit * 2 + label.implicitWidth
        implicitHeight: 7 * App.theme.look.unit
        color: "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.CheckBox
        Accessible.name: text
        Accessible.checked: on
        Keys.onSpacePressed: toggle.toggled(!on)
        Keys.onReturnPressed: toggle.toggled(!on)
        TapHandler { onTapped: toggle.toggled(!toggle.on) }
        Rectangle {
            id: track
            anchors.verticalCenter: parent.verticalCenter
            width: 22
            height: 12
            radius: 6
            color: toggle.on ? App.theme.look.accent : App.theme.look.hairlineStrong
            border.width: toggle.activeFocus ? 1 : 0
            border.color: App.theme.look.accent
            Rectangle {
                x: toggle.on ? track.width - width - 2 : 2
                y: 2
                width: 8
                height: 8
                radius: 4
                color: toggle.on ? App.theme.look.accentForeground : App.theme.look.body
            }
        }
        Text {
            id: label
            anchors.left: track.right
            anchors.leftMargin: 2 * App.theme.look.unit
            anchors.verticalCenter: parent.verticalCenter
            text: toggle.text
            color: App.theme.look.text
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
        opacity: enabled ? 1 : 0.5
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

    // A one-line text field in a well. It commits on Enter or when focus leaves; the model
    // decides, and a refused text goes back to the value.
    component Field: Rectangle {
        id: field
        property string value
        property string placeholder
        property bool secret: false
        property alias input: textInput
        signal committed(string text)
        Layout.fillWidth: true
        implicitWidth: 60 * App.theme.look.unit
        implicitHeight: 8 * App.theme.look.unit
        radius: App.theme.look.smallCorner
        color: App.theme.look.well
        border.width: 1
        border.color: textInput.activeFocus ? App.theme.look.accent : App.theme.look.hairlineStrong
        function commit() {
            if (textInput.text !== field.value)
                field.committed(textInput.text)
            textInput.text = Qt.binding(() => field.secret ? "" : field.value)
        }
        TextInput {
            id: textInput
            anchors { left: parent.left; right: parent.right; leftMargin: 2 * App.theme.look.unit; rightMargin: 2 * App.theme.look.unit
                      verticalCenter: parent.verticalCenter }
            clip: true
            selectByMouse: true
            text: field.secret ? "" : field.value
            echoMode: field.secret ? TextInput.Password : TextInput.Normal
            color: App.theme.look.text
            selectionColor: App.theme.look.accent
            selectedTextColor: App.theme.look.accentForeground
            font.pixelSize: App.theme.look.textSize
            Accessible.role: Accessible.EditableText
            Accessible.name: field.placeholder
            onAccepted: field.commit()
            onActiveFocusChanged: if (!activeFocus && text !== (field.secret ? "" : field.value)) field.commit()
            Keys.onEscapePressed: event => {
                if (textInput.text !== (field.secret ? "" : field.value)) {
                    textInput.text = Qt.binding(() => field.secret ? "" : field.value)
                    event.accepted = true
                } else {
                    event.accepted = false
                }
            }
            Text {
                visible: textInput.text === ""
                anchors.verticalCenter: parent.verticalCenter
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: field.placeholder
                color: App.theme.look.label
                font: textInput.font
            }
        }
    }

    // A folder setting: the folder in effect, Choose… (the portal), and Default when chosen.
    component FolderSetting: Setting {
        id: folder
        required property var host // the panel: its model and pickers
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
                objectName: folder.key + "-choose"
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
}
