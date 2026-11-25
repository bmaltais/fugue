pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../kit"
import "../canvas"

// The Providers page of the settings panel (spec 8.2, 8.8), on the Drift kit: a search box, then one line per service in a list
// (glyph, name, where it runs, its status, its switch), one of them open at a time to show its settings in a tray, and a closed row
// "Add a service" that holds the well-known services (provider_presets.h). The search filters the list and the catalogue as you
// type (SettingsModel.providerMatches). A cloud service opens on its Key; its name, address and rare settings sit under Advanced. A key
// shows only whether it is set: typing one sends it to the model once, and it is never shown (the eye shows what is being typed, not
// what is stored). Adding a service opens its row with the focus where the person types next. Defaults (no providers.json yet) say
// "Built in" until the first change writes them all.
//
// The list is made again on every change, so the page keeps its own state by the instance's NAME, not its place: which row is open
// (`expandedName`), which have Advanced open (`advancedOpen`), and where the focus was (`keep`, put back by `restoreFocus()` when
// the row is made again). A row that moves keeps its state because the name moves with it.
ColumnLayout {
    id: page
    required property var host // the SettingsPanel: its model and apply()
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property SettingsModel model: host.model
    spacing: 0

    property string expandedName // the one open row ("" none)
    property var advancedOpen: ({}) // name -> true
    property bool catalogueOpen: false
    property string revealName // the row that is opening scrolls itself into view while it grows
    property var keep: null // {name, part}: the focus to put back once the list is made again
    property string tabName // the last header used: the only one Tab stops on
    property int removeRow: -1

    readonly property string query: search.text.trim()
    readonly property bool searching: query !== ""
    readonly property bool locked: model.providersProblem !== ""
    readonly property var presets: model.providerPresets()
    readonly property var shownPresets: searching ? presets.filter(p => model.providerMatches(query, p)) : presets
    readonly property var shownNames: model.providers.filter(i => !searching || model.providerMatches(query, i)).map(i => i.name)
    readonly property bool nothing: searching && shownNames.length === 0 && shownPresets.length === 0
    readonly property string tabRow: shownNames.includes(tabName) ? tabName : (shownNames.length > 0 ? shownNames[0] : "")

    // The shared "where" column: as wide as the widest word, at most 22u.
    function whereWord(placement) { return (presets.find(p => p.placement === placement) ?? {placementText: ""}).placementText }
    TextMetrics { id: tmCloud; font.pixelSize: page.look.labelSize; text: page.whereWord("cloud") }
    TextMetrics { id: tmMachine; font.pixelSize: page.look.labelSize; text: page.whereWord("machine") }
    TextMetrics { id: tmNetwork; font.pixelSize: page.look.labelSize; text: page.whereWord("network") }
    readonly property real detailWidth: Math.min(22 * u, Math.ceil(Math.max(tmCloud.advanceWidth, tmMachine.advanceWidth, tmNetwork.advanceWidth)))

    // --- state, kept by name ------------------------------------------------------------------

    function indexOfName(name) {
        for (let i = 0; i < rows.count; ++i) {
            const r = rows.itemAt(i) as KitDisclosureRow
            if (r && r.rowKey === name)
                return i
        }
        return -1
    }
    function rowByName(name) { return rows.itemAt(indexOfName(name)) as KitDisclosureRow }
    function findPart(item, objectName) {
        if (item.objectName === objectName && item.visible)
            return item
        for (const child of item.children) {
            const found = page.findPart(child, objectName)
            if (found)
                return found
        }
        return null
    }
    // Remembers where the focus is, to put it back when the rows are made again (a change rebuilds them all).
    function noteFocus() {
        keep = null
        const focused = page.Window.activeFocusItem
        let part = ""
        for (let p = focused; p; p = p.parent) {
            if (part === "" && p.objectName && p.objectName.startsWith("provider"))
                part = p.objectName.replace(/-\d+/, "-#")
            const r = p as KitDisclosureRow
            if (r && r.rowKey !== "") {
                keep = {name: r.rowKey, part: part === "" || focused === r.header ? "header" : part}
                return
            }
        }
    }
    function restoreFocus() {
        if (!keep)
            return
        const at = indexOfName(keep.name)
        const part = keep.part
        keep = null
        if (at < 0)
            return
        const r = rows.itemAt(at) as KitDisclosureRow
        const target = part === "header" ? r.header : (findPart(r, part.replace("-#", "-" + at)) ?? r.header)
        const focusable = target.input ?? target // a field's text input, else the item itself
        focusable.forceActiveFocus(Qt.OtherFocusReason)
    }
    // Runs a model change that rebuilds the rows, keeping the focus. `rename` {from, to} carries the state of a row whose name changes.
    function edit(call, rename) {
        noteFocus()
        if (rename)
            page.carry(rename.from, rename.to)
        const ok = page.host.apply(call())
        if (!ok && rename)
            page.carry(rename.to, rename.from)
        return ok
    }
    function carry(from, to) {
        if (expandedName === from)
            expandedName = to
        if (tabName === from)
            tabName = to
        if (keep && keep.name === from)
            keep = {name: to, part: keep.part}
        const next = Object.assign({}, advancedOpen)
        if (from in next) {
            next[to] = next[from]
            delete next[from]
        }
        advancedOpen = next
    }
    function setExpanded(name, open) {
        if (!open) {
            if (expandedName === name)
                expandedName = ""
            return
        }
        if (expandedName === name)
            return
        const was = rowByName(expandedName)
        const now = rowByName(name)
        // The row that was open closes at once; when it is above, the page drops by its body so the new row stays under the pointer.
        if (was) {
            was.animate = false
            if (now && was.y < now.y)
                page.host.scrollBy(-was.shown)
        }
        revealName = name
        revealTimer.restart()
        expandedName = name
        if (was)
            Qt.callLater(() => was.animate = true)
    }
    Timer { id: revealTimer; interval: 400; onTriggered: page.revealName = "" }

    function uniqueName(base) {
        const names = model.providers.map(i => i.name)
        let name = base
        for (let n = 2; names.includes(name); ++n)
            name = base + " " + n
        return name
    }
    // Opens a service's row and puts the focus on a part of it.
    function show(name, part) {
        page.setExpanded(name, true)
        keep = {name: name, part: part}
        Qt.callLater(page.restoreFocus)
    }
    function addPreset(id) {
        const p = presets.find(x => x.id === id)
        if (!p)
            return
        const there = p.keyUrl !== "" ? model.providers.findIndex(x => x.preset === id) : -1
        if (there >= 0) { // one account per cloud service: go to its row
            page.show(model.providers[there].name, "providerKey-#")
            return
        }
        const name = uniqueName(p.name)
        const wasOpen = expandedName
        search.text = ""
        expandedName = name
        revealName = name
        revealTimer.restart()
        keep = {name: name, part: p.needsAddress ? "providerAddress-#" : p.needsKey ? "providerKey-#" : "providerAddress-#"}
        if (!page.host.apply(model.addPreset(id))) {
            expandedName = wasOpen
            keep = null
            return
        }
        catalogueOpen = false
    }
    function askRemove(row, hasKey) {
        const at = shownNames.indexOf(model.providers[row].name)
        const next = at >= 0 ? (shownNames[at + 1] ?? shownNames[at - 1] ?? "") : ""
        if (!hasKey) {
            page.removeNow(row, next)
            return
        }
        page.removeRow = row
        removeAsk.next = next
        removeAsk.open()
    }
    function removeNow(row, next) {
        const name = model.providers[row].name
        if (expandedName === name)
            expandedName = ""
        keep = next !== "" ? {name: next, part: "header"} : null
        if (page.host.apply(model.removeProvider(row))) {
            if (next === "")
                search.focusField()
        } else {
            keep = null
        }
    }
    function focusName(name) {
        const r = rowByName(name)
        if (r)
            r.focusHeader()
    }
    // Up, Down, Home and End on a header: the next visible header, none past the ends (Down from the last goes on to "Add a service").
    function step(name, delta) {
        const at = shownNames.indexOf(name)
        if (at < 0)
            return
        if (Math.abs(delta) >= 1000)
            focusName(shownNames[delta < 0 ? 0 : shownNames.length - 1])
        else if (at + delta >= shownNames.length)
            catalogue.focusHeader()
        else if (at + delta >= 0)
            focusName(shownNames[at + delta])
    }
    function focusFirst() {
        if (shownNames.length > 0) {
            focusName(shownNames[0])
            return
        }
        for (const p of shownPresets) {
            const chip = findPart(catalogueBody, "providerPresetAdd-" + p.id) ?? findPart(catalogueBody, "providerPresetShow-" + p.id)
            if (chip) {
                chip.forceActiveFocus(Qt.TabFocusReason)
                return
            }
        }
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

    Shortcut { // checked against the command registry: Ctrl+F is unused there
        sequence: "Ctrl+F"
        enabled: page.visible
        onActivated: search.focusField()
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

    KitSearch {
        id: search
        objectName: "providerSearch"
        Layout.fillWidth: true
        Layout.topMargin: 3 * page.u
        look: page.look
        placeholder: qsTr("Search services")
        // Typing opens the catalogue, so a service that is not on the list yet shows up in the same search.
        onTextChanged: if (text.trim() !== "") page.catalogueOpen = true
        onDown: page.focusFirst()
        onAccepted: page.focusFirst()
    }

    // --- the list ------------------------------------------------------------------------------

    KitGroup {
        objectName: "providerList"
        Layout.fillWidth: true
        Layout.topMargin: 3 * page.u
        look: page.look
        KitRow {
            objectName: "providerEmpty"
            look: page.look
            visible: page.model.providers.length === 0 && !page.nothing
            label: qsTr("No service yet")
            hint: qsTr("Add one below")
        }
        KitRow {
            objectName: "providerNoMatch"
            look: page.look
            visible: page.nothing || (page.searching && page.model.providers.length > 0 && page.shownNames.length === 0)
            label: page.nothing ? qsTr("Nothing matches \"%1\"").arg(page.query) : qsTr("None of your services match")
            KitChip {
                objectName: "providerClearSearch"
                look: page.look
                visible: page.nothing
                text: qsTr("Clear search")
                onTapped: { search.text = ""; search.focusField() }
            }
        }
        Repeater {
            id: rows
            model: page.model.providers
            onItemAdded: (index, item) => {
                if (page.keep && (item as KitDisclosureRow).rowKey === page.keep.name)
                    Qt.callLater(page.restoreFocus)
            }
            delegate: KitDisclosureRow {
                id: row
                required property var modelData
                required property int index
                readonly property bool cloud: modelData.cloud === true
                readonly property string keyUrl: modelData.keyUrl ?? ""
                readonly property bool advancedShown: page.advancedOpen[modelData.name] === true
                readonly property var plainFields: modelData.fields.filter(f => !f.advanced)
                readonly property var advancedFields: modelData.fields.filter(f => f.advanced)
                property bool reveal: false

                objectName: "providerRow-" + index
                rowKey: modelData.name
                statusName: "providerStatus-" + index
                bodyName: "providerBody-" + index
                look: page.look
                visible: !page.searching || page.shownNames.includes(modelData.name)
                label: modelData.name
                glyph: modelData.placement === "cloud" ? "cloud" : modelData.placement === "machine" ? "monitor" : "server"
                detail: modelData.placementText
                detailWidth: page.detailWidth
                status: modelData.statusText
                statusStrong: modelData.statusStrong
                expanded: page.expandedName === modelData.name
                tabStop: page.tabRow === modelData.name
                bodyEnabled: !page.locked
                onExpandRequested: value => page.setExpanded(modelData.name, value)
                onStepRequested: delta => page.step(modelData.name, delta)
                onMoveRequested: delta => {
                    const to = row.index + delta
                    if (!page.searching && !page.locked && to >= 0 && to < page.model.providers.length)
                        page.edit(() => page.model.moveProvider(row.index, to))
                }
                onShownChanged: if (page.revealName === modelData.name) page.host.showAll(row, header.height + shown)
                Connections {
                    target: row.header
                    function onActiveFocusChanged() { if (row.header.activeFocus) page.tabName = row.modelData.name }
                }

                trailing: SettingsParts.Toggle {
                    objectName: "providerEnabled-" + row.index
                    enabled: !page.locked
                    activeFocusOnTab: enabled && row.tabStop
                    text: qsTr("Use %1").arg(row.modelData.name)
                    on: row.modelData.enabled
                    onToggled: value => page.edit(() => page.model.setProviderField(row.index, "enabled", value))
                }

                // The rows two places share: a cloud service keeps them under Advanced, the others show them first.
                Component {
                    id: nameRow
                    KitRow {
                        look: page.look
                        label: qsTr("Name")
                        SettingsParts.Field {
                            objectName: "providerName-" + row.index
                            label: qsTr("Name")
                            value: row.modelData.name
                            placeholder: qsTr("Name")
                            onCommitted: text => page.edit(() => page.model.setProviderField(row.index, "name", text),
                                                           {from: row.modelData.name, to: text.trim()})
                        }
                    }
                }
                Component {
                    id: addressRow
                    KitRow {
                        look: page.look
                        label: qsTr("Address")
                        hint: row.cloud ? qsTr("Empty is the service's own") : ""
                        SettingsParts.Field {
                            objectName: "providerAddress-" + row.index
                            label: qsTr("Address")
                            value: row.modelData.address
                            placeholder: qsTr("Default")
                            onCommitted: text => page.edit(() => page.model.setProviderField(row.index, "address", text))
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
                            objectName: "providerField-" + row.index + "-" + fr.modelData.name
                            label: fr.modelData.label
                            secret: fr.modelData.secret
                            value: fr.modelData.secret ? "" : fr.modelData.value
                            placeholder: fr.modelData.secret ? (fr.modelData.set ? qsTr("Set; type a new one to replace it") : qsTr("None")) : ""
                            onCommitted: text => {
                                if (!fr.modelData.secret || text !== "")
                                    page.edit(() => page.model.setProviderField(row.index, fr.modelData.name, text))
                            }
                        }
                        SettingsParts.Toggle {
                            id: fieldSwitch
                            visible: fr.modelData.isBool
                            objectName: "providerField-" + row.index + "-" + fr.modelData.name
                            text: fr.modelData.label
                            on: fr.modelData.value === "true"
                            onToggled: value => page.edit(() => page.model.setProviderField(row.index, fr.modelData.name, value))
                        }
                    }
                }

                // What it is sent (a cloud service) or where it runs, and whether it is only the built-in default.
                Item {
                    width: parent.width
                    height: line.height + 3 * page.u
                    Flow {
                        id: line
                        x: page.look.params.rowPadLeft * page.u
                        y: 1.5 * page.u
                        width: parent.width - (page.look.params.rowPadLeft + page.look.params.rowPadRight) * page.u
                        Text {
                            objectName: "providerPrivacy-" + row.index
                            width: Math.min(implicitWidth, line.width)
                            wrapMode: Text.Wrap
                            text: row.modelData.privacy !== "" ? row.modelData.privacy : page.placement(row.modelData)
                            textFormat: Text.PlainText
                            color: page.look.label
                            font.pixelSize: page.look.labelSize
                        }
                        Text {
                            objectName: "providerDefault-" + row.index
                            visible: !row.modelData.saved
                            text: qsTr(" · Built in")
                            textFormat: Text.PlainText
                            color: page.look.label
                            font.pixelSize: page.look.labelSize
                        }
                    }
                }
                Repeater { model: row.cloud ? 0 : 1; delegate: addressRow }
                Repeater { model: row.cloud ? 0 : 1; delegate: nameRow }
                KitRow {
                    look: page.look
                    label: qsTr("Key")
                    hint: row.modelData.keySet ? qsTr("Saved")
                          : !row.modelData.keyRequired ? qsTr("Optional")
                          : row.modelData.enabled ? qsTr("Needed before %1 can run").arg(row.modelData.name) : ""
                    SettingsParts.Field {
                        id: keyField
                        objectName: "providerKey-" + row.index
                        secret: true
                        reveal: row.reveal
                        label: qsTr("Key")
                        placeholder: row.modelData.keySet ? "••••••••••••" : row.cloud ? qsTr("Paste your %1 key").arg(row.modelData.name) : qsTr("No key")
                        onCommitted: text => {
                            row.reveal = false
                            if (text !== "")
                                page.edit(() => page.model.setProviderField(row.index, "key", text))
                        }
                        adornment: Item { // the eye: shows what is being typed, inside the well (as KitSearch holds its clear button)
                            id: eye
                            objectName: "providerKeyShow-" + row.index
                            width: 6 * page.u
                            height: 6 * page.u
                            activeFocusOnTab: true
                            Accessible.role: Accessible.CheckBox
                            Accessible.name: row.reveal ? qsTr("Hide the key as you type it") : qsTr("Show the key as you type it")
                            Accessible.checkable: true
                            Accessible.checked: row.reveal
                            Accessible.onPressAction: row.reveal = !row.reveal
                            Keys.onSpacePressed: row.reveal = !row.reveal
                            Keys.onReturnPressed: row.reveal = !row.reveal
                            Rectangle {
                                anchors.fill: parent
                                radius: width / 2
                                color: "transparent"
                                border.width: eye.activeFocus && FocusInput.keyboard ? 2 : 0
                                border.color: page.look.accent
                            }
                            Glyph { anchors.centerIn: parent; look: page.look; name: "eye"; color: row.reveal || eyeHover.hovered ? page.look.text : page.look.label }
                            HoverHandler { id: eyeHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { gesturePolicy: TapHandler.WithinBounds; onTapped: row.reveal = !row.reveal }
                            ToolTip.visible: eyeHover.hovered
                            ToolTip.delay: page.look.tooltipDelay
                            ToolTip.text: eye.Accessible.name
                        }
                    }
                }
                KitRow {
                    look: page.look
                    visible: row.modelData.keySet || row.keyUrl !== ""
                    label: row.keyUrl !== "" ? qsTr("Get a key") : qsTr("Saved key")
                    hint: row.keyUrl.replace(/^https?:\/\//, "")
                    KitChip {
                        objectName: "providerKeyClear-" + row.index
                        look: page.look
                        visible: row.modelData.keySet
                        text: qsTr("Clear key")
                        onTapped: page.edit(() => page.model.setProviderField(row.index, "key", ""))
                    }
                    KitChip {
                        objectName: "providerKeyGet-" + row.index
                        look: page.look
                        visible: row.keyUrl !== ""
                        text: qsTr("Get a key")
                        tip: row.keyUrl
                        onTapped: Qt.openUrlExternally(row.keyUrl)
                    }
                }
                // The module's own settings; the rare ones, and a cloud service's name and address, sit under Advanced.
                Repeater { model: row.plainFields; delegate: fieldRow }
                KitRow {
                    look: page.look
                    visible: row.advancedFields.length > 0 || row.cloud
                    label: qsTr("Advanced")
                    target: advancedChip
                    KitChip {
                        id: advancedChip
                        objectName: "providerAdvanced-" + row.index
                        look: page.look
                        pill: true
                        glyph: row.advancedShown ? "chevron-up" : "chevron-down"
                        tip: row.advancedShown ? qsTr("Hide the advanced settings") : qsTr("Show the advanced settings")
                        current: row.advancedShown
                        function toggle() { tapped() }
                        onTapped: {
                            const next = Object.assign({}, page.advancedOpen)
                            next[row.modelData.name] = !row.advancedShown
                            page.advancedOpen = next
                        }
                    }
                }
                Repeater { model: row.cloud && row.advancedShown ? 1 : 0; delegate: nameRow }
                Repeater { model: row.cloud && row.advancedShown ? 1 : 0; delegate: addressRow }
                Repeater { model: row.advancedShown ? row.advancedFields : []; delegate: fieldRow }
                // The footer: the order the runs try the services in, at the start; Remove, in its own place, at the end.
                KitSeparator { look: page.look }
                Item {
                    width: parent.width
                    height: 10 * page.u
                    Row {
                        anchors { left: parent.left; leftMargin: page.look.params.rowPadLeft * page.u; verticalCenter: parent.verticalCenter }
                        spacing: 2 * page.u
                        visible: page.model.providers.length > 1
                        KitChip {
                            objectName: "providerMoveUp-" + row.index
                            look: page.look
                            glyph: "arrow-up"
                            round: true
                            dimmed: row.index === 0 || page.searching || page.locked
                            tip: page.locked ? qsTr("Fix providers.json first") : page.searching ? qsTr("Clear the search to change the order")
                                 : row.index === 0 ? qsTr("Already first") : qsTr("Move up")
                            onTapped: if (!dimmed) page.edit(() => page.model.moveProvider(row.index, row.index - 1))
                        }
                        KitChip {
                            objectName: "providerMoveDown-" + row.index
                            look: page.look
                            glyph: "arrow-down"
                            round: true
                            dimmed: row.index >= page.model.providers.length - 1 || page.searching || page.locked
                            tip: page.locked ? qsTr("Fix providers.json first") : page.searching ? qsTr("Clear the search to change the order")
                                 : row.index >= page.model.providers.length - 1 ? qsTr("Already last") : qsTr("Move down")
                            onTapped: if (!dimmed) page.edit(() => page.model.moveProvider(row.index, row.index + 1))
                        }
                    }
                    KitChip {
                        objectName: "providerRemove-" + row.index
                        anchors { right: parent.right; rightMargin: page.look.params.rowPadRight * page.u; verticalCenter: parent.verticalCenter }
                        look: page.look
                        danger: true
                        enabled: !page.locked
                        text: qsTr("Remove")
                        tip: page.locked ? qsTr("Fix providers.json first") : ""
                        Accessible.name: qsTr("Remove %1").arg(row.modelData.name)
                        // A stored key is lost with it: ask, with Keep as the answer a stray Enter gives. Nothing stored, nothing lost.
                        onTapped: page.askRemove(row.index, row.modelData.keySet)
                    }
                }
            }
        }
    }

    // --- adding --------------------------------------------------------------------------------

    KitGroup {
        Layout.fillWidth: true
        Layout.topMargin: 5 * page.u
        look: page.look
        visible: !page.nothing
        KitDisclosureRow {
            id: catalogue
            objectName: "providerCatalogue"
            look: page.look
            label: qsTr("Add a service")
            glyph: "plus"
            status: page.searching ? qsTr("%n matches", "", page.shownPresets.length) : qsTr("%n services", "", page.presets.length)
            tray: false
            expanded: page.catalogueOpen || page.model.providers.length === 0
            onExpandRequested: value => page.catalogueOpen = value
            onStepRequested: delta => {
                if (delta < 0 && page.shownNames.length > 0)
                    page.focusName(page.shownNames[page.shownNames.length - 1])
            }

            Item { // the body: dim headings, a row per service
                id: catalogueBody
                width: parent.width
                height: catalogueColumn.height
                Column {
                    id: catalogueColumn
                    width: parent.width
                    Repeater {
                        model: ["cloud", "own"]
                        delegate: Column {
                            id: part
                            required property string modelData
                            readonly property var items: page.shownPresets.filter(p => p.group === modelData)
                            width: parent.width
                            visible: items.length > 0
                            Text {
                                x: page.look.params.rowPadLeft * page.u
                                topPadding: 3 * page.u
                                bottomPadding: 1.5 * page.u
                                text: part.items.length > 0 ? part.items[0].groupTitle : ""
                                textFormat: Text.PlainText
                                color: page.look.label
                                font.pixelSize: page.look.labelSize
                            }
                            Repeater {
                                model: part.items
                                delegate: KitRow {
                                    id: preset
                                    required property var modelData
                                    readonly property bool added: modelData.keyUrl !== "" && page.model.providers.some(x => x.preset === modelData.id)
                                    objectName: "providerPreset-" + modelData.id
                                    look: page.look
                                    label: modelData.name
                                    hint: modelData.description
                                    target: added ? showChip : addChip
                                    KitBadge {
                                        look: page.look
                                        visible: preset.modelData.freeTier
                                        text: qsTr("Free tier")
                                    }
                                    KitBadge {
                                        objectName: "providerPresetAdded-" + preset.modelData.id
                                        look: page.look
                                        visible: preset.added
                                        text: qsTr("Added")
                                    }
                                    KitChip {
                                        id: showChip
                                        objectName: "providerPresetShow-" + preset.modelData.id
                                        look: page.look
                                        visible: preset.added
                                        text: qsTr("Show")
                                        function toggle() { tapped() }
                                        onTapped: page.show(page.model.providers.find(x => x.preset === preset.modelData.id).name, "providerKey-#")
                                    }
                                    KitChip {
                                        id: addChip
                                        objectName: "providerPresetAdd-" + preset.modelData.id
                                        look: page.look
                                        visible: !preset.added
                                        enabled: !page.locked
                                        text: qsTr("Add")
                                        tip: page.locked ? qsTr("Fix providers.json first") : ""
                                        function toggle() { tapped() }
                                        onTapped: page.addPreset(preset.modelData.id)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Question {
        id: removeAsk
        objectName: "providerRemoveAsk"
        property string next
        readonly property var instance: page.removeRow >= 0 ? page.model.providers[page.removeRow] : null
        onOpened: keepButton.forceActiveFocus()
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
                    id: keepButton
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
                        page.removeNow(page.removeRow, removeAsk.next)
                    }
                }
            }
        }
    }
}
