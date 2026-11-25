pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../kit"
import "../canvas"
import "../canvas/widgets"

// The Providers page of the settings panel (spec 8.2, 8.8), on the Drift kit: per instance a title line (the name, with its Enabled
// switch at the end), one dim line under it (where it runs, or for a cloud service what it is sent), a KitGroup of KitRows, and a
// group of its own for Remove. A cloud service opens on its Key; its name, address and rare settings sit under Advanced. A key shows
// only whether it is set: typing one sends it to the model once, and it is never shown (the eye shows what is being typed, not what
// is stored). "Add a service…" lists the well-known services (provider_presets.h) by group; choosing one adds the instance and puts
// the focus where the person types next. Defaults (no providers.json yet) say "Built in" until the first change writes them all.
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
    // Which cards have Advanced open, by place: the cards are made again on every change and keep it open.
    property var advancedOpen: ({})
    property int removeRow: -1
    property int goIndex: -1 // a card to take the focus now (a service already added was chosen again)

    // The presets, as the menu lists them: a heading per group, then each service with its description; a cloud service already
    // on the page says "Added" (choosing it goes to its card).
    function presetEntries() {
        const out = []
        let group = ""
        for (const p of page.model.providerPresets()) {
            if (p.group !== group) {
                group = p.group
                out.push({text: p.groupTitle, heading: true, enabled: false})
            }
            const there = p.keyUrl !== "" && page.model.providers.some(x => x.preset === p.id)
            out.push({text: p.name, note: p.description, value: p.id, detail: there ? qsTr("Added") : ""})
        }
        return out
    }
    function addPreset(id) {
        const p = page.model.providerPresets().find(x => x.id === id)
        if (!p)
            return
        page.added = true
        const there = p.keyUrl !== "" ? page.model.providers.findIndex(x => x.preset === id) : -1
        if (there >= 0) { // one account per cloud service: go to its card
            page.focusField = "key"
            page.goIndex = there
            return
        }
        // Set before the call: the cards are made again inside it, and the new one reads these as it appears.
        page.focusField = p.needsAddress ? "address" : p.needsKey ? "key" : "address"
        page.focusIndex = page.model.providers.length
        if (!page.host.apply(page.model.addPreset(id)))
            page.focusIndex = -1
    }
    function askRemove(row, hasKey) {
        if (!hasKey) {
            page.host.apply(page.model.removeProvider(row))
            return
        }
        page.removeRow = row
        removeAsk.open()
    }
    // Where an instance runs, in plain words (a cloud service says what it is sent instead).
    function placement(modelData) {
        const address = modelData.address
        const local = address === "" || /^https?:\/\/(localhost|127\.|\[::1\])/.test(address)
        if (local)
            return qsTr("On this machine")
        if (modelData.type === "omarchy" || modelData.type === "fugue-helper")
            return qsTr("On your network")
        return qsTr("At %1").arg(address.replace(/^https?:\/\//, "").replace(/\/.*$/, ""))
    }

    Text {
        Layout.fillWidth: true
        Layout.leftMargin: 2 * page.u
        text: qsTr("Services that run models, tried top to bottom.")
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

    // The dropdown, made where it is wanted: above the cards, or in the empty state's row.
    Component {
        id: addControl
        KitDropdown {
            id: addService
            objectName: "providerAddService"
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
                width: 64 * page.u
                maxRows: 40 // all the services and both headings show without a scroll
                onAboutToShow: page.added = false
                onPicked: id => page.addPreset(id)
                onClosed: if (!page.added) addService.forceActiveFocus()
            }
        }
    }
    Loader {
        Layout.topMargin: 3 * page.u
        Layout.leftMargin: 2 * page.u
        active: page.model.providers.length > 0
        visible: active
        sourceComponent: addControl
    }
    // Nothing left: one row says so and holds the dropdown.
    KitGroup {
        Layout.fillWidth: true
        Layout.topMargin: 3 * page.u
        look: page.look
        visible: page.model.providers.length === 0 && page.model.providersProblem === ""
        KitRow {
            look: page.look
            label: qsTr("No service yet")
            Loader { sourceComponent: addControl }
        }
    }

    Repeater {
        id: cards
        model: page.model.providers
        delegate: Column {
            id: card
            required property var modelData
            required property int index
            objectName: "provider-" + index
            Layout.fillWidth: true
            readonly property string privacy: card.modelData.privacy ?? ""
            readonly property string keyUrl: card.modelData.keyUrl ?? ""
            readonly property bool cloud: card.modelData.cloud === true
            readonly property bool advancedShown: page.advancedOpen[card.index] === true
            readonly property var plainFields: card.modelData.fields.filter(f => !f.advanced)
            readonly property var advancedFields: card.modelData.fields.filter(f => f.advanced)
            property bool reveal: false
            property var addressInput: null

            function takeFocus() {
                const input = page.focusField === "address" && card.addressInput ? card.addressInput.input : keyField.input
                input.forceActiveFocus(Qt.TabFocusReason)
                page.focusIndex = -1
                page.host.reveal(card)
            }
            Component.onCompleted: if (card.index === page.focusIndex) Qt.callLater(card.takeFocus)
            readonly property bool going: page.goIndex === card.index
            onGoingChanged: if (going) {
                card.takeFocus()
                page.goIndex = -1
            }

            // The rows two places share: a cloud service keeps them under Advanced, the others show them first.
            Component {
                id: nameRow
                KitRow {
                    look: page.look
                    label: qsTr("Name")
                    SettingsParts.Field {
                        objectName: "providerName-" + card.index
                        label: qsTr("Name")
                        value: card.modelData.name
                        placeholder: qsTr("Name")
                        onCommitted: text => page.host.apply(page.model.setProviderField(card.index, "name", text))
                    }
                }
            }
            Component {
                id: addressRow
                KitRow {
                    look: page.look
                    label: qsTr("Address")
                    hint: card.cloud ? qsTr("Empty is the service's own") : ""
                    SettingsParts.Field {
                        id: addressField
                        objectName: "providerAddress-" + card.index
                        label: qsTr("Address")
                        value: card.modelData.address
                        placeholder: qsTr("Default")
                        onCommitted: text => page.host.apply(page.model.setProviderField(card.index, "address", text))
                        Component.onCompleted: card.addressInput = addressField
                    }
                }
            }
            // A setting of the module: a text field, or a switch.
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

            // The title line: the name, and its switch at the end.
            Item {
                width: parent.width
                height: title.implicitHeight
                KitSection {
                    id: title
                    look: page.look
                    text: card.modelData.name
                    width: parent.width - 14 * page.u
                    topPadding: card.index === 0 ? 3 * page.u : 5 * page.u
                    bottomPadding: 0.5 * page.u
                }
                SettingsParts.Toggle {
                    objectName: "providerEnabled-" + card.index
                    anchors.right: parent.right
                    anchors.rightMargin: 2 * page.u
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: (title.topPadding - title.bottomPadding) / 2
                    text: qsTr("Use %1").arg(card.modelData.name)
                    on: card.modelData.enabled
                    onToggled: value => page.host.apply(page.model.setProviderField(card.index, "enabled", value))
                }
            }
            Row {
                width: parent.width
                leftPadding: 2 * page.u
                bottomPadding: 1.5 * page.u
                spacing: 0
                Text {
                    objectName: "providerPrivacy-" + card.index
                    text: card.privacy !== "" ? card.privacy : page.placement(card.modelData)
                    textFormat: Text.PlainText
                    color: page.look.label
                    font.pixelSize: page.look.labelSize
                }
                Text {
                    objectName: "providerDefault-" + card.index
                    visible: !card.modelData.saved
                    text: qsTr(" · Built in")
                    textFormat: Text.PlainText
                    color: page.look.label
                    font.pixelSize: page.look.labelSize
                }
            }

            KitGroup {
                look: page.look
                Repeater { model: card.cloud ? 0 : 1; delegate: nameRow }
                Repeater { model: card.cloud ? 0 : 1; delegate: addressRow }
                KitRow {
                    look: page.look
                    label: qsTr("Key")
                    hint: card.modelData.keySet ? qsTr("Saved")
                          : card.modelData.enabled && card.cloud ? qsTr("Needed before %1 can run").arg(card.modelData.name) : ""
                    SettingsParts.Field {
                        id: keyField
                        objectName: "providerKey-" + card.index
                        secret: true
                        reveal: card.reveal
                        label: qsTr("Key")
                        placeholder: card.modelData.keySet ? "••••••••••••" : card.cloud ? qsTr("Paste your %1 key").arg(card.modelData.name) : qsTr("No key")
                        onCommitted: text => {
                            card.reveal = false
                            if (text !== "")
                                page.host.apply(page.model.setProviderField(card.index, "key", text))
                        }
                        adornment: Item { // the eye: shows what is being typed, inside the well (as KitSearch holds its clear button)
                            id: eye
                            objectName: "providerKeyShow-" + card.index
                            width: 6 * page.u
                            height: 6 * page.u
                            activeFocusOnTab: true
                            Accessible.role: Accessible.CheckBox
                            Accessible.name: card.reveal ? qsTr("Hide the key as you type it") : qsTr("Show the key as you type it")
                            Accessible.checkable: true
                            Accessible.checked: card.reveal
                            Accessible.onPressAction: card.reveal = !card.reveal
                            Keys.onSpacePressed: card.reveal = !card.reveal
                            Keys.onReturnPressed: card.reveal = !card.reveal
                            Rectangle {
                                anchors.fill: parent
                                radius: width / 2
                                color: "transparent"
                                border.width: eye.activeFocus && FocusInput.keyboard ? 2 : 0
                                border.color: page.look.accent
                            }
                            Glyph { anchors.centerIn: parent; look: page.look; name: "eye"; color: card.reveal || eyeHover.hovered ? page.look.text : page.look.label }
                            HoverHandler { id: eyeHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { gesturePolicy: TapHandler.WithinBounds; onTapped: card.reveal = !card.reveal }
                            ToolTip.visible: eyeHover.hovered
                            ToolTip.delay: page.look.tooltipDelay
                            ToolTip.text: eye.Accessible.name
                        }
                    }
                }
                KitRow {
                    look: page.look
                    visible: card.modelData.keySet || card.keyUrl !== ""
                    label: card.keyUrl !== "" ? qsTr("Get a key") : qsTr("Saved key")
                    hint: card.keyUrl.replace(/^https?:\/\//, "")
                    KitChip {
                        objectName: "providerKeyClear-" + card.index
                        look: page.look
                        visible: card.modelData.keySet
                        text: qsTr("Clear key")
                        onTapped: page.host.apply(page.model.setProviderField(card.index, "key", ""))
                    }
                    KitChip {
                        objectName: "providerKeyGet-" + card.index
                        look: page.look
                        visible: card.keyUrl !== ""
                        text: qsTr("Get a key")
                        tip: card.keyUrl
                        onTapped: Qt.openUrlExternally(card.keyUrl)
                    }
                }
                // The module's own settings; the rare ones, and a cloud service's name and address, sit under Advanced.
                Repeater { model: card.plainFields; delegate: fieldRow }
                KitRow {
                    look: page.look
                    visible: card.advancedFields.length > 0 || card.cloud
                    label: qsTr("Advanced")
                    target: advancedChip
                    KitChip {
                        id: advancedChip
                        objectName: "providerAdvanced-" + card.index
                        look: page.look
                        pill: true
                        glyph: card.advancedShown ? "chevron-up" : "chevron-down"
                        tip: card.advancedShown ? qsTr("Hide the advanced settings") : qsTr("Show the advanced settings")
                        current: card.advancedShown
                        function toggle() { tapped() }
                        onTapped: {
                            const next = Object.assign({}, page.advancedOpen)
                            next[card.index] = !card.advancedShown
                            page.advancedOpen = next
                        }
                    }
                }
                Repeater { model: card.cloud && card.advancedShown ? 1 : 0; delegate: nameRow }
                Repeater { model: card.cloud && card.advancedShown ? 1 : 0; delegate: addressRow }
                Repeater { model: card.advancedShown ? card.advancedFields : []; delegate: fieldRow }
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
                        dimmed: card.index === 0
                        tip: card.index === 0 ? qsTr("Already first") : qsTr("Move up")
                        onTapped: if (card.index > 0) page.host.apply(page.model.moveProvider(card.index, card.index - 1))
                    }
                    KitChip {
                        objectName: "providerMoveDown-" + card.index
                        look: page.look
                        glyph: "arrow-down"
                        round: true
                        dimmed: card.index >= page.model.providers.length - 1
                        tip: dimmed ? qsTr("Already last") : qsTr("Move down")
                        onTapped: if (!dimmed) page.host.apply(page.model.moveProvider(card.index, card.index + 1))
                    }
                }
            }
            // Removing is a chunk of its own, after twice the gap of the rows.
            Item { width: 1; height: 5 * page.u }
            KitGroup {
                look: page.look
                KitRow {
                    look: page.look
                    label: qsTr("Remove this service")
                    KitChip {
                        objectName: "providerRemove-" + card.index
                        look: page.look
                        danger: true
                        text: qsTr("Remove")
                        Accessible.name: qsTr("Remove %1").arg(card.modelData.name)
                        // A stored key is lost with it: ask, with Keep as the answer a stray Enter gives. Nothing stored, nothing lost.
                        onTapped: page.askRemove(card.index, card.modelData.keySet)
                    }
                }
            }
        }
    }

    Question {
        id: removeAsk
        objectName: "providerRemoveAsk"
        readonly property var instance: page.removeRow >= 0 ? page.model.providers[page.removeRow] : null
        onOpened: keep.forceActiveFocus()
        ColumnLayout {
            anchors.fill: parent
            spacing: 3 * page.u
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Remove %1? Its saved key is deleted from providers.json.").arg(removeAsk.instance ? removeAsk.instance.name : "")
            }
            QuestionAnswers {
                ThemedButton {
                    id: keep
                    objectName: "providerRemoveKeep"
                    text: qsTr("Keep it")
                    highlighted: true
                    onClicked: removeAsk.close()
                }
                ThemedButton {
                    objectName: "providerRemoveGo"
                    text: qsTr("Remove")
                    danger: true
                    onClicked: {
                        removeAsk.close()
                        page.host.apply(page.model.removeProvider(page.removeRow))
                    }
                }
            }
        }
    }
}
