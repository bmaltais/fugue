pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue

// The Providers page of the settings panel (spec 8.2, 8.8): the instances in preference
// order, as the runs and the model picker see them. Defaults (no providers.json yet) show as
// "default, not saved"; the first change writes them all. A key or other secret field shows
// only whether it is set: typing one sends it to the model once, and it is never shown.
ColumnLayout {
    id: page
    required property var host // the SettingsPanel: its model and apply()
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property SettingsModel model: host.model
    spacing: 4 * u

    SettingsParts.Setting {
        title: qsTr("Providers")
        note: qsTr("What runs a model, tried top to bottom. An empty address is the module's own (this machine for Omarchy).")
    }

    Text {
        objectName: "providersProblem"
        visible: page.model.providersProblem !== ""
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: page.look.error
        font.pixelSize: page.look.labelSize
        text: qsTr("%1 cannot be read, so nothing here can change until it is fixed: %2")
                  .arg(page.model.providersPath).arg(page.model.providersProblem)
    }

    Repeater {
        model: page.model.providers
        delegate: Rectangle {
            id: card
            required property var modelData
            required property int index
            objectName: "provider-" + index
            Layout.fillWidth: true
            implicitHeight: cardColumn.implicitHeight + 6 * page.u
            readonly property bool glass: page.look.params.panelTint > 0.001
            radius: Math.min(page.look.params.cornerGroup * page.look.unit, 0.3 * implicitHeight)
            color: glass ? (page.look.light ? Qt.rgba(0, 0, 0, page.look.params.groupFillAlphaLight) : Qt.rgba(1, 1, 1, page.look.params.groupFillAlpha)) : page.look.kitGroup
            border.width: page.look.params.borders === "hairline" && glass ? 1 : 0
            border.color: page.look.light ? Qt.rgba(0, 0, 0, 0.07) : Qt.rgba(1, 1, 1, 0.06)
            readonly property bool editable: page.model.providersProblem === ""

            ColumnLayout {
                id: cardColumn
                x: 3 * page.u
                y: 3 * page.u
                width: card.width - 6 * page.u
                spacing: 2 * page.u

                RowLayout {
                    Layout.fillWidth: true
                    spacing: page.u
                    // Enabled.
                    SettingsParts.Toggle {
                        objectName: "providerEnabled-" + card.index
                        text: qsTr("Use %1").arg(card.modelData.name)
                        on: card.modelData.enabled
                        onToggled: value => page.host.apply(page.model.setProviderField(card.index, "enabled", value))
                    }
                    SettingsParts.Field {
                        objectName: "providerName-" + card.index
                        value: card.modelData.name
                        placeholder: qsTr("Name")
                        onCommitted: text => page.host.apply(page.model.setProviderField(card.index, "name", text))
                    }
                    Text {
                        text: card.modelData.type
                        color: page.look.label
                        font.pixelSize: page.look.captionSize
                    }
                    Text {
                        objectName: "providerDefault-" + card.index
                        visible: !card.modelData.saved
                        text: qsTr("default, not saved")
                        color: page.look.label
                        font.pixelSize: page.look.captionSize
                        font.italic: true
                    }
                    SettingsParts.SmallButton {
                        glyph: "arrow-up"
                        visible: card.index > 0
                        Accessible.name: qsTr("Move up")
                        onClicked: page.host.apply(page.model.moveProvider(card.index, card.index - 1))
                    }
                    SettingsParts.SmallButton {
                        glyph: "arrow-down"
                        visible: card.index < page.model.providers.length - 1
                        Accessible.name: qsTr("Move down")
                        onClicked: page.host.apply(page.model.moveProvider(card.index, card.index + 1))
                    }
                    SettingsParts.SmallButton {
                        objectName: "providerRemove-" + card.index
                        glyph: "x"
                        Accessible.name: qsTr("Remove %1").arg(card.modelData.name)
                        onClicked: page.host.apply(page.model.removeProvider(card.index))
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 2 * page.u
                    rowSpacing: page.u
                    Text { text: qsTr("Address"); color: page.look.label; font.pixelSize: page.look.labelSize }
                    SettingsParts.Field {
                        objectName: "providerAddress-" + card.index
                        value: card.modelData.address
                        placeholder: qsTr("The module's own")
                        onCommitted: text => page.host.apply(page.model.setProviderField(card.index, "address", text))
                    }
                    Text { text: qsTr("Key"); color: page.look.label; font.pixelSize: page.look.labelSize }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: page.u
                        SettingsParts.Field {
                            objectName: "providerKey-" + card.index
                            secret: true
                            placeholder: card.modelData.keySet ? qsTr("Set; type a new one to replace it") : qsTr("None")
                            onCommitted: text => {
                                if (text !== "")
                                    page.host.apply(page.model.setProviderField(card.index, "key", text))
                            }
                        }
                        SettingsParts.SmallButton {
                            objectName: "providerKeyClear-" + card.index
                            visible: card.modelData.keySet
                            text: qsTr("Clear")
                            onClicked: page.host.apply(page.model.setProviderField(card.index, "key", ""))
                        }
                    }
                    // The module's own settings.
                    Repeater {
                        model: card.modelData.fields
                        delegate: Text {
                            required property var modelData
                            Layout.row: 2 + index
                            Layout.column: 0
                            required property int index
                            text: modelData.name
                            color: page.look.label
                            font.pixelSize: page.look.labelSize
                        }
                    }
                    Repeater {
                        model: card.modelData.fields
                        delegate: SettingsParts.Field {
                            required property var modelData
                            required property int index
                            Layout.row: 2 + index
                            Layout.column: 1
                            objectName: "providerField-" + card.index + "-" + modelData.name
                            secret: modelData.secret
                            value: modelData.secret ? "" : modelData.value
                            placeholder: modelData.secret ? (modelData.set ? qsTr("Set; type a new one to replace it") : qsTr("None")) : ""
                            onCommitted: text => {
                                if (!modelData.secret || text !== "")
                                    page.host.apply(page.model.setProviderField(card.index, modelData.name, text))
                            }
                        }
                    }
                }
            }
        }
    }

    Row {
        spacing: page.u
        Repeater {
            model: page.model.providerTypes
            delegate: SettingsParts.SmallButton {
                required property string modelData
                objectName: "providerAdd-" + modelData
                text: qsTr("Add %1 provider").arg(modelData)
                onClicked: page.host.apply(page.model.addProvider(modelData))
            }
        }
    }
}
