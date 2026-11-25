import QtQuick
import QtQuick.Layouts
import Fugue
import "../kit"

// The settings panel's building blocks (#51, #464), as inline components, on the Drift kit (docs/canvas-look.md 20): a Setting is
// a KitGroup card (the title, what it does, where its value comes from, then its control), a Toggle a KitToggle, a SmallButton a
// raised KitChip, a Field a recessed KitWell. They only report what the user did; the panel and its pages call the model. (Not
// Bound: another file cannot make a bound inline component.)
QtObject {
    // A titled setting: a rounded card with the name and what it does on top and its control under them, as the kit's group is.
    // `head` takes a control that belongs on the title's line (a toggle). With `key` and `origin` ("inherited" or "override",
    // from SettingsModel.origins) it also says where the value comes from (spec 1.4.2): "Inherited from Default", or, for the
    // project's own value over the Default's, a button that gives it up (useDefaultRequested).
    component Setting: Rectangle {
        id: setting
        property string title
        property string note
        property string key
        property string origin
        default property alias content: inner.data
        property alias head: headSlot.data
        signal useDefaultRequested()
        readonly property CanvasLook look: App.theme.look
        Layout.fillWidth: true
        implicitHeight: outer.implicitHeight + 2 * look.params.rowPadY * look.unit + 2 * look.unit
        // A tinted panel shows through the card; a plain one gets the opaque group fill (KitGroup's rule).
        readonly property bool glass: look.params.panelTint > 0.001
        radius: Math.min(look.params.cornerGroup * look.unit, 0.3 * height)
        color: glass ? (look.light ? Qt.rgba(0, 0, 0, look.params.groupFillAlphaLight) : Qt.rgba(1, 1, 1, look.params.groupFillAlpha)) : look.kitGroup
        border.width: look.params.borders === "hairline" && glass ? 1 : 0
        border.color: look.light ? Qt.rgba(0, 0, 0, 0.07) : Qt.rgba(1, 1, 1, 0.06)
        ColumnLayout {
            id: outer
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter
                      leftMargin: setting.look.params.rowPadLeft * setting.look.unit; rightMargin: setting.look.params.rowPadRight * setting.look.unit }
            spacing: setting.look.unit
            RowLayout {
                Layout.fillWidth: true
                spacing: 2 * setting.look.unit
                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: setting.title
                    textFormat: Text.PlainText
                    color: setting.look.text
                    font.pixelSize: Math.round(setting.look.textSize * 7 / 6)
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }
                Text {
                    objectName: "origin-" + setting.key
                    visible: setting.origin === "inherited"
                    text: qsTr("Inherited from Default")
                    color: setting.look.accent
                    font.pixelSize: setting.look.captionSize
                }
                SmallButton {
                    objectName: "useDefault-" + setting.key
                    visible: setting.origin === "override"
                    text: qsTr("Use Default's value")
                    onClicked: setting.useDefaultRequested()
                }
                Item { Layout.fillWidth: true }
                Row {
                    id: headSlot
                    spacing: 2 * setting.look.unit
                }
            }
            Text {
                visible: text !== ""
                Layout.fillWidth: true
                text: setting.note
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: setting.look.label
                font.pixelSize: setting.look.labelSize
            }
            ColumnLayout {
                id: inner
                Layout.fillWidth: true
                spacing: setting.look.unit
            }
        }
    }

    // A card of a list (a provider, a pack, a cache kind): the kit group's translucent fill and corner, as a Setting has.
    component Card: Rectangle {
        readonly property CanvasLook look: App.theme.look
        readonly property bool glass: look.params.panelTint > 0.001
        radius: Math.min(look.params.cornerGroup * look.unit, 0.3 * implicitHeight)
        color: glass ? (look.light ? Qt.rgba(0, 0, 0, look.params.groupFillAlphaLight) : Qt.rgba(1, 1, 1, look.params.groupFillAlpha)) : look.kitGroup
        border.width: look.params.borders === "hairline" && glass ? 1 : 0
        border.color: look.light ? Qt.rgba(0, 0, 0, 0.07) : Qt.rgba(1, 1, 1, 0.06)
    }

    // An on/off switch: the kit's toggle in the window's tone. `on` is the old name of `checked`, kept for the pages and tests that
    // read it; the Setting's head (or a row's trailing slot) gives it its place.
    component Toggle: KitToggle {
        property bool on: false
        checked: on
        look: App.theme.look
    }

    // A small button of a form: a raised kit chip. `highlighted` is the form's primary, the accent; `danger` a removal, in the red tint
    // (docs/canvas-look.md 11) while it can act. A thin alias, so the settings keep their names and `clicked`.
    component SmallButton: KitChip {
        property bool highlighted: false
        signal clicked()
        look: App.theme.look
        primary: highlighted
        onTapped: clicked()
    }

    // A one-line text field in a recessed well. It commits on Enter or when focus leaves; the model decides, and a refused text
    // goes back to the value.
    component Field: Item {
        id: field
        property string value
        property string placeholder
        property bool secret: false
        property bool reveal: false // a secret typed shows as typed (the eye chip); never what is stored, which this field does not know
        property string label // the screen reader's name; the placeholder when empty
        property alias input: textInput
        property alias adornment: adornmentSlot.data // a small control at the end of the well (the key's eye)
        signal committed(string text)
        Layout.fillWidth: true
        implicitWidth: 60 * App.theme.look.unit // the control column of a kit row
        implicitHeight: 8 * App.theme.look.unit
        function commit() {
            // The text goes back to the stored value first: a change that builds the page again destroys this field, which loses the
            // focus, which commits (a second time, with the same text) from inside the first.
            const typed = textInput.text
            textInput.text = Qt.binding(() => field.secret ? "" : field.value)
            if (typed !== (field.secret ? "" : field.value))
                field.committed(typed)
        }
        KitWell {
            look: App.theme.look
            focused: textInput.activeFocus
        }
        Row {
            id: adornmentSlot
            anchors { right: parent.right; rightMargin: 1 * App.theme.look.unit; verticalCenter: parent.verticalCenter }
        }
        TextInput {
            id: textInput
            anchors { left: parent.left; right: parent.right; leftMargin: 2.5 * App.theme.look.unit; rightMargin: 2.5 * App.theme.look.unit + adornmentSlot.width
                      verticalCenter: parent.verticalCenter }
            clip: true
            selectByMouse: true
            text: field.secret ? "" : field.value
            echoMode: field.secret && !field.reveal ? TextInput.Password : TextInput.Normal
            color: App.theme.look.text
            selectionColor: App.theme.look.accent
            selectedTextColor: App.theme.look.accentForeground
            font.pixelSize: App.theme.look.textSize
            Accessible.role: Accessible.EditableText
            Accessible.name: field.label !== "" ? field.label : field.placeholder
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
