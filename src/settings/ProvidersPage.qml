pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../kit"
import "../canvas/widgets"

// The Providers page of the settings panel (spec 8.2, 8.8), on the Drift kit: a KitSection (the name) and a KitGroup of KitRows per
// instance, in preference order, as the runs and the model picker see them. Defaults (no providers.json yet) show as "default, not
// saved"; the first change writes them all. A key or other secret field shows only whether it is set: typing one sends it to the
// model once, and it is never shown (the eye chip shows what is being typed, not what is stored). "Add a service…" lists the
// well-known services (provider_presets.h) by group; choosing one adds the instance and puts the focus where the person types next.
ColumnLayout {
    id: page
    required property var host // the SettingsPanel: its model and apply()
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property SettingsModel model: host.model
    spacing: 0

    // The card made for the service just added takes the focus when it appears (the list is made again on every change).
    property int focusIndex: -1
    property string focusField: "key"
    property bool added: false // the menu closed because a service was chosen, so the focus goes to its card, not back to the dropdown

    // The presets, as the menu lists them: a heading per group, then each service with its description.
    function presetEntries() {
        const out = []
        let group = ""
        for (const p of page.model.providerPresets()) {
            if (p.group !== group) {
                group = p.group
                out.push({text: p.groupTitle, heading: true, enabled: false})
            }
            out.push({text: p.name, note: p.description, value: p.id})
        }
        return out
    }
    function addPreset(id) {
        const p = page.model.providerPresets().find(x => x.id === id)
        if (!p)
            return
        // Set before the call: the cards are made again inside it, and the new one reads these as it appears.
        page.focusField = p.needsAddress ? "address" : p.needsKey ? "key" : "address"
        page.focusIndex = page.model.providers.length
        page.added = true
        if (!page.host.apply(page.model.addPreset(id)))
            page.focusIndex = -1
    }

    Text {
        Layout.fillWidth: true
        Layout.leftMargin: 2 * page.u
        text: qsTr("What runs a model, tried top to bottom. An empty address is the module's own (this machine for Omarchy).")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: page.look.label
        font.pixelSize: page.look.labelSize
    }

    Text {
        objectName: "providersProblem"
        visible: page.model.providersProblem !== ""
        Layout.fillWidth: true
        Layout.topMargin: 2 * page.u
        wrapMode: Text.Wrap
        color: page.look.error
        font.pixelSize: page.look.labelSize
        text: qsTr("%1 cannot be read, so nothing here can change until it is fixed: %2")
                  .arg(page.model.providersPath).arg(page.model.providersProblem)
    }

    KitDropdown {
        id: addService
        objectName: "providerAddService"
        Layout.topMargin: 3 * page.u
        Layout.leftMargin: 2 * page.u
        look: page.look
        enabled: page.model.providersProblem === ""
        text: qsTr("Add a service…")
        elide: Text.ElideRight
        open: addMenu.opened
        Accessible.name: qsTr("Add a service")
        onTapped: {
            const at = addService.mapToItem(Overlay.overlay, 0, addService.height)
            addMenu.wantX = at.x
            addMenu.wantY = at.y
            addMenu.anchorHeight = addService.height
            addMenu.entries = page.presetEntries()
            addMenu.open()
        }
        WidgetMenu {
            id: addMenu
            objectName: "providerAddMenu"
            look: page.look
            parent: Overlay.overlay
            width: 44 * page.u
            onAboutToShow: page.added = false
            onPicked: id => page.addPreset(id)
            onClosed: if (!page.added) addService.forceActiveFocus()
        }
    }

    Repeater {
        model: page.model.providers
        delegate: Column {
            id: card
            required property var modelData
            required property int index
            objectName: "provider-" + index
            Layout.fillWidth: true
            readonly property bool editable: page.model.providersProblem === ""
            readonly property string privacy: card.modelData.privacy ?? ""
            readonly property string keyUrl: card.modelData.keyUrl ?? ""
            property bool reveal: false
            property bool advancedOpen: false
            readonly property bool addressAdvanced: card.modelData.addressAdvanced === true
            readonly property var plainFields: card.modelData.fields.filter(f => !f.advanced)
            readonly property var advancedFields: card.modelData.fields.filter(f => f.advanced)

            function takeFocus() {
                const target = page.focusField === "address" ? addressField : keyField
                target.input.forceActiveFocus(Qt.TabFocusReason)
                page.focusIndex = -1
                page.host.reveal(card)
            }
            Component.onCompleted: if (card.index === page.focusIndex) Qt.callLater(card.takeFocus)


            Component {
                id: fieldRow
                KitRow {
                    id: fr
                    required property var modelData
                    look: page.look
                    label: modelData.label
                    hint: modelData.hint
                    target: modelData.isBool ? fieldSwitch : null
                    SettingsParts.Field {
                        visible: !fr.modelData.isBool
                        objectName: "providerField-" + card.index + "-" + fr.modelData.name
                        implicitWidth: 34 * page.u
                        label: fr.modelData.label
                        secret: fr.modelData.secret
                        value: fr.modelData.secret ? "" : fr.modelData.value
                        placeholder: fr.modelData.secret ? (fr.modelData.set ? qsTr("Set; type a new one to replace it") : qsTr("None")) : ""
                        onCommitted: text => {
                            if (!fr.modelData.secret || text !== "")
                                page.host.apply(page.model.setProviderField(card.index, fr.modelData.name, text))
                        }
                    }
                    SettingsParts.Toggle {
                        id: fieldSwitch
                        visible: fr.modelData.isBool
                        objectName: "providerField-" + card.index + "-" + fr.modelData.name
                        text: fr.modelData.label
                        on: fr.modelData.value === "true"
                        onToggled: value => page.host.apply(page.model.setProviderField(card.index, fr.modelData.name, value))
                    }
                }
            }
            KitSection {
                look: page.look
                text: card.modelData.name
                topPadding: card.index === 0 ? 3 * page.u : 5 * page.u
                bottomPadding: card.privacy !== "" ? 0 : 1.5 * page.u
            }
            Text {
                objectName: "providerPrivacy-" + card.index
                visible: card.privacy !== ""
                width: parent.width
                leftPadding: 2 * page.u
                bottomPadding: 1.5 * page.u
                text: card.privacy
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: page.look.label
                font.pixelSize: page.look.labelSize
            }
            KitGroup {
                look: page.look
                // Enabled.
                KitRow {
                    look: page.look
                    label: qsTr("Enabled")
                    hint: card.modelData.type
                    target: enabledToggle
                    Text {
                        objectName: "providerDefault-" + card.index
                        visible: !card.modelData.saved
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("default, not saved")
                        textFormat: Text.PlainText
                        color: page.look.label
                        font.pixelSize: page.look.captionSize
                        font.italic: true
                    }
                    SettingsParts.Toggle {
                        id: enabledToggle
                        objectName: "providerEnabled-" + card.index
                        text: qsTr("Use %1").arg(card.modelData.name)
                        on: card.modelData.enabled
                        onToggled: value => page.host.apply(page.model.setProviderField(card.index, "enabled", value))
                    }
                }
                KitRow {
                    look: page.look
                    label: qsTr("Name")
                    SettingsParts.Field {
                        objectName: "providerName-" + card.index
                        implicitWidth: 34 * page.u
                        label: qsTr("Name")
                        value: card.modelData.name
                        placeholder: qsTr("Name")
                        onCommitted: text => page.host.apply(page.model.setProviderField(card.index, "name", text))
                    }
                }
                KitRow {
                    look: page.look
                    label: qsTr("Address")
                    visible: !card.addressAdvanced
                    SettingsParts.Field {
                        id: addressField
                        objectName: "providerAddress-" + card.index
                        implicitWidth: 34 * page.u
                        label: qsTr("Address")
                        value: card.modelData.address
                        placeholder: qsTr("The module's own")
                        onCommitted: text => page.host.apply(page.model.setProviderField(card.index, "address", text))
                    }
                }
                KitRow {
                    look: page.look
                    label: qsTr("Key")
                    SettingsParts.Field {
                        id: keyField
                        objectName: "providerKey-" + card.index
                        implicitWidth: 34 * page.u
                        secret: true
                        reveal: card.reveal
                        label: qsTr("Key")
                        placeholder: card.modelData.keySet ? qsTr("Set; type a new one to replace it") : qsTr("None")
                        onCommitted: text => {
                            card.reveal = false
                            if (text !== "")
                                page.host.apply(page.model.setProviderField(card.index, "key", text))
                        }
                    }
                    KitChip {
                        objectName: "providerKeyShow-" + card.index
                        look: page.look
                        glyph: "eye"
                        round: true
                        current: card.reveal
                        tip: card.reveal ? qsTr("Hide the key as you type it") : qsTr("Show the key as you type it")
                        Accessible.role: Accessible.CheckBox
                        Accessible.checkable: true
                        Accessible.checked: card.reveal
                        onTapped: card.reveal = !card.reveal
                    }
                    KitChip {
                        objectName: "providerKeyGet-" + card.index
                        look: page.look
                        visible: card.keyUrl !== ""
                        text: qsTr("Get a key")
                        tip: card.keyUrl
                        onTapped: Qt.openUrlExternally(card.keyUrl)
                    }
                    KitChip {
                        objectName: "providerKeyClear-" + card.index
                        look: page.look
                        visible: card.modelData.keySet
                        text: qsTr("Clear key")
                        onTapped: page.host.apply(page.model.setProviderField(card.index, "key", ""))
                    }
                }
                // The module's own settings; a service whose card folds some of them (fal) puts the rest under Advanced.
                Repeater {
                    model: card.plainFields
                    delegate: fieldRow
                }
                KitRow {
                    look: page.look
                    visible: card.advancedFields.length > 0 || card.addressAdvanced
                    label: qsTr("Advanced")
                    target: advancedChip
                    KitChip {
                        id: advancedChip
                        objectName: "providerAdvanced-" + card.index
                        look: page.look
                        pill: true
                        glyph: card.advancedOpen ? "chevron-up" : "chevron-down"
                        tip: card.advancedOpen ? qsTr("Hide the advanced settings") : qsTr("Show the advanced settings")
                        current: card.advancedOpen
                        function toggle() { tapped() }
                        onTapped: card.advancedOpen = !card.advancedOpen
                    }
                }
                KitRow {
                    look: page.look
                    visible: card.addressAdvanced && card.advancedOpen
                    label: qsTr("Address")
                    hint: qsTr("Empty is the service's own")
                    SettingsParts.Field {
                        objectName: "providerAddress-" + card.index
                        implicitWidth: 34 * page.u
                        label: qsTr("Address")
                        value: card.modelData.address
                        placeholder: qsTr("The service's own")
                        onCommitted: text => page.host.apply(page.model.setProviderField(card.index, "address", text))
                    }
                }
                Repeater {
                    model: card.advancedOpen ? card.advancedFields : []
                    delegate: fieldRow
                }
                // The order the runs try the services in.
                KitRow {
                    look: page.look
                    label: qsTr("Order")
                    hint: qsTr("Services are tried top to bottom")
                    visible: page.model.providers.length > 1
                    KitChip {
                        objectName: "providerMoveUp-" + card.index
                        look: page.look
                        glyph: "arrow-up"
                        round: true
                        enabled: card.index > 0
                        tip: qsTr("Move up")
                        onTapped: page.host.apply(page.model.moveProvider(card.index, card.index - 1))
                    }
                    KitChip {
                        objectName: "providerMoveDown-" + card.index
                        look: page.look
                        glyph: "arrow-down"
                        round: true
                        enabled: card.index < page.model.providers.length - 1
                        tip: qsTr("Move down")
                        onTapped: page.host.apply(page.model.moveProvider(card.index, card.index + 1))
                    }
                }
                // Removing is a chunk of its own, last.
                KitRow {
                    look: page.look
                    label: qsTr("Remove this service")
                    hint: qsTr("Its key is forgotten")
                    KitChip {
                        objectName: "providerRemove-" + card.index
                        look: page.look
                        danger: true
                        text: qsTr("Remove")
                        Accessible.name: qsTr("Remove %1").arg(card.modelData.name)
                        onTapped: page.host.apply(page.model.removeProvider(card.index))
                    }
                }
            }
        }
    }
}
