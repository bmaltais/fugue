pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue
import "../kit"

// The Packs page of the settings panel (spec 6.6-6.8, #25), on the Drift kit and shaped as the Providers page is: a search box, then
// one line per pack in a list (glyph, name, version, trust, and Trust... when it is not yet), one of them open at a time to show its
// folder, its keys, what the load said and Update and Remove in a tray, a closed row "Add a pack", and under them the library's groups.
// The search filters packs and groups as you type. Every change goes through a question the person answers here (at the top of the
// page): the model installs, trusts, removes or runs a setup only after a yes, and shows exactly what it will do, with what each
// pack's sandbox lets it do (6.7.1) and, loudly, a pack the person turned the sandbox off for.
//
// The list is made again on every change, so the page keeps its state by the pack's ID, not its place: which row is open
// (`expandedKey`) and which row had the focus (`keepKey`, put back by `restoreFocus()` when the rows are made again).
ColumnLayout {
    id: page
    required property var host // the SettingsPanel
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property PacksModel model: host.model.packs
    spacing: 0

    property string expandedKey // the one open row ("" none)
    property string tabKey // the last header used: the only one Tab stops on
    property string revealKey // the row that is opening scrolls itself into view while it grows
    property string keepKey // the row whose header gets the focus back once the list is made again
    property bool addOpen: false

    readonly property string query: search.text.trim().toLowerCase()
    readonly property bool searching: query !== ""
    function packMatches(p) { return [p.name, p.id, p.version, p.path].join("\n").toLowerCase().includes(query) }
    function groupMatches(g) { return [g.name, g.id].join("\n").toLowerCase().includes(query) }
    readonly property var shownKeys: model.packs.filter(p => !searching || packMatches(p)).map(p => p.path) // a pack's path is the one thing no two share (two majors of one id, a broken pack)
    readonly property var shownLibrary: model.library.filter(g => !searching || groupMatches(g))
    readonly property bool nothing: searching && shownKeys.length === 0 && shownLibrary.length === 0
    readonly property string tabRow: shownKeys.includes(tabKey) ? tabKey : (shownKeys.length > 0 ? shownKeys[0] : "")

    // The status column's width, shared: as wide as the widest word. The trust slot is as wide as the Trust... chip while any pack asks for it.
    TextMetrics { id: tmTrusted; font.pixelSize: page.look.labelSize; text: qsTr("trusted") }
    TextMetrics { id: tmNotTrusted; font.pixelSize: page.look.labelSize; text: qsTr("not trusted") }
    TextMetrics { id: tmDisabled; font.pixelSize: page.look.labelSize; text: qsTr("disabled") }
    TextMetrics { id: tmInheritedNot; font.pixelSize: page.look.labelSize; text: qsTr("inherited, %1").arg(qsTr("not trusted")) }
    TextMetrics { id: tmInheritedDis; font.pixelSize: page.look.labelSize; text: qsTr("inherited, %1").arg(qsTr("disabled")) }
    readonly property real statusWidth: Math.ceil(Math.max(tmTrusted.advanceWidth, tmNotTrusted.advanceWidth, tmDisabled.advanceWidth,
                                                            model.packs.some(p => p.inherited) ? Math.max(tmInheritedNot.advanceWidth, tmInheritedDis.advanceWidth) : 0))
    // What the row says about trust; a pack of the Default project says so too, so the Trust... beside it is never read alone.
    function statusOf(p) {
        const word = !p.enabled ? qsTr("disabled") : !p.trusted ? qsTr("not trusted") : p.sandboxed ? qsTr("trusted") : qsTr("trusted, NOT sandboxed")
        return p.inherited ? qsTr("inherited, %1").arg(word) : word
    }
    KitChip { id: trustMeasure; look: page.look; visible: false; text: qsTr("Trust…") }
    readonly property real trustSlot: model.packs.some(p => p.enabled && !p.trusted) ? Math.ceil(trustMeasure.implicitWidth) : 0

    function indexOfKey(id) {
        for (let i = 0; i < rows.count; ++i) {
            const r = rows.itemAt(i) as KitDisclosureRow
            if (r && r.rowKey === id)
                return i
        }
        return -1
    }
    function rowByKey(id) { return rows.itemAt(indexOfKey(id)) as KitDisclosureRow }
    // Remembers which row holds the focus, to put it back when the rows are made again (a change rebuilds them all).
    function noteFocus() {
        keepKey = ""
        for (let p = page.Window.activeFocusItem; p; p = p.parent) {
            const r = p as KitDisclosureRow
            if (r && r.rowKey !== "") {
                keepKey = r.rowKey
                return
            }
        }
    }
    function restoreFocus() {
        const r = keepKey !== "" ? rowByKey(keepKey) : null
        keepKey = ""
        if (r)
            r.focusHeader()
    }
    function setExpanded(id, open) {
        if (!open) {
            if (expandedKey === id)
                expandedKey = ""
            return
        }
        if (expandedKey === id)
            return
        const was = rowByKey(expandedKey)
        const now = rowByKey(id)
        // The row that was open closes at once; when it is above, the page drops by its body so the new row stays under the pointer.
        if (was) {
            was.animate = false
            if (now && was.y < now.y)
                page.host.scrollBy(-was.shown)
        }
        revealKey = id
        revealTimer.restart()
        expandedKey = id
        if (was)
            Qt.callLater(() => was.animate = true)
    }
    Timer { id: revealTimer; interval: 400; onTriggered: page.revealKey = "" }

    function focusKey(id) {
        const r = rowByKey(id)
        if (r)
            r.focusHeader()
    }
    // Up, Down, Home and End on a header: the next visible header, none past the ends (Down from the last goes on to "Add a pack").
    function step(id, delta) {
        const at = shownKeys.indexOf(id)
        if (at < 0)
            return
        if (Math.abs(delta) >= 1000)
            focusKey(shownKeys[delta < 0 ? 0 : shownKeys.length - 1])
        else if (at + delta >= shownKeys.length)
            addRow.focusHeader()
        else if (at + delta >= 0)
            focusKey(shownKeys[at + delta])
    }
    function focusFirst() {
        if (shownKeys.length > 0)
            focusKey(shownKeys[0])
        else if (shownLibrary.length > 0 || !nothing)
            addRow.focusHeader()
    }

    Shortcut { // checked against the command registry: Ctrl+F is unused there
        sequence: "Ctrl+F"
        enabled: page.visible
        onActivated: search.focusField()
    }

    Text {
        Layout.fillWidth: true
        Layout.leftMargin: 2 * page.u
        text: qsTr("Nodes come in packs. A pack's programs run in a sandbox that gives them only what the pack declares. A sandbox is not trust: trust only packs you would run anyway.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: page.look.label
        font.pixelSize: page.look.labelSize
    }

    // The question: what will happen, and the person says yes or no.
    Rectangle {
        id: ask
        objectName: "packQuestion"
        visible: page.model.question.kind !== undefined
        readonly property var q: page.model.question
        Layout.fillWidth: true
        Layout.topMargin: 3 * page.u
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
                textFormat: Text.PlainText
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
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: page.look.label
                font.pixelSize: page.look.labelSize
                text: (ask.q.inheritedWarning ? ask.q.inheritedWarning + "\n" : "") + (ask.q.kind === "setup"
                      ? (ask.q.sandboxed ? qsTr("%1\nIn %2, this command runs in the sandbox, with the network on:\n%3")
                                         : qsTr("%1\nIn %2, this command runs as you (NOT sandboxed):\n%3")).arg(ask.q.description).arg(ask.q.folder).arg(ask.q.command)
                      : qsTr("%1\nRuns as: %2\nDeclares: %3%4\n%5")
                            .arg(ask.q.source !== "" ? ask.q.id + "\n" + qsTr("From %1").arg(ask.q.source) : ask.q.id)
                            .arg(ask.q.runtime && ask.q.runtime.length ? ask.q.runtime.join(", ") : qsTr("no nodes"))
                            .arg(ask.q.permissions && ask.q.permissions.length ? ask.q.permissions.join(", ") : qsTr("nothing (network, files, GPU)"))
                            .arg((ask.q.secrets && ask.q.secrets.length ? "\n" + qsTr("Reads secrets: %1").arg(ask.q.secrets.join(", ")) : "")
                                 + (ask.q.secretsSet && ask.q.secretsSet.length ? "\n" + qsTr("Already set: %1").arg(ask.q.secretsSet.join(", ")) : "")
                                 + (ask.q.secretsOtherSource && ask.q.secretsOtherSource.length
                                    ? "\n" + qsTr("%1 already set for this pack id from another source; this pack does not get it.").arg(ask.q.secretsOtherSource.join(", ")) : "")
                                 + (ask.q.secrets && ask.q.secrets.length && !ask.q.sandboxed ? "\n" + qsTr("Isolation between packs is advisory: a trusted pack's programs run as you and can read Fugue's config folder.") : ""))
                            .arg(ask.q.sandboxed ? qsTr("Runs in a sandbox: it sees only its own folder, the files it is given and what it declares.")
                                                 : qsTr("NOT sandboxed: this pack's program runs as you.")))
            }
            // No first, the answer that acts last at the right (KitDialogButtons); a removal is the danger tint and No the safe,
            // highlighted answer (D8).
            KitDialogButtons {
                Layout.topMargin: page.u
                SettingsParts.SmallButton {
                    objectName: "packNo"
                    dialogRole: "cancel"
                    enabled: page.model.questionReady
                    highlighted: ask.q.kind === "remove"
                    text: qsTr("No")
                    onClicked: page.model.answer(false)
                }
                SettingsParts.SmallButton {
                    objectName: "packYes"
                    dialogRole: ask.q.kind === "remove" ? "destructive" : "accept"
                    enabled: page.model.questionReady
                    highlighted: ask.q.kind !== "remove"
                    danger: ask.q.kind === "remove"
                    text: ask.q.kind === "setup" ? qsTr("Run it") : ask.q.kind === "remove" ? qsTr("Remove") : qsTr("Yes")
                    onClicked: page.model.answer(true)
                }
            }
        }
    }


    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 3 * page.u
        spacing: 2 * page.u
        KitSearch {
            id: search
            objectName: "packSearch"
            Layout.fillWidth: true
            look: page.look
            placeholder: qsTr("Search packs")
            onDown: page.focusFirst()
            onAccepted: page.focusFirst()
        }
        KitChip {
            objectName: "packsUpdateAll"
            look: page.look
            enabled: !page.model.busy
            text: qsTr("Update all")
            onTapped: page.model.update("")
        }
    }

    // --- the list ------------------------------------------------------------------------------

    KitGroup {
        objectName: "packList"
        Layout.fillWidth: true
        Layout.topMargin: 3 * page.u
        look: page.look
        KitRow {
            objectName: "packEmpty"
            look: page.look
            visible: page.model.packs.length === 0 && !page.nothing
            label: qsTr("No packs installed")
            hint: qsTr("Add one below")
        }
        KitRow {
            objectName: "packNoMatch"
            look: page.look
            visible: page.nothing || (page.searching && page.model.packs.length > 0 && page.shownKeys.length === 0)
            label: page.nothing ? qsTr("Nothing matches \"%1\"").arg(search.text.trim()) : qsTr("None of your packs match")
            KitChip {
                objectName: "packClearSearch"
                look: page.look
                visible: page.nothing
                text: qsTr("Clear search")
                onTapped: { search.text = ""; search.focusField() }
            }
        }
        Repeater {
            id: rows
            model: page.model.packs
            onItemAdded: (index, item) => {
                if (page.keepKey !== "" && (item as KitDisclosureRow).rowKey === page.keepKey)
                    Qt.callLater(page.restoreFocus)
            }
            delegate: KitDisclosureRow {
                id: row
                required property var modelData
                required property int index
                readonly property string pid: modelData.id !== "" ? modelData.id : modelData.folder // names the parts for tests; a pack that did not load has no id
                readonly property string pkey: modelData.path // the row's state key: unique
                readonly property bool attention: !modelData.enabled || !modelData.trusted

                objectName: "pack-" + pid
                rowKey: pkey
                statusName: "packState-" + pid
                bodyName: "packBody-" + pid
                look: page.look
                visible: !page.searching || page.shownKeys.includes(pkey)
                label: modelData.name !== "" ? modelData.name : modelData.id !== "" ? modelData.id : modelData.folder.split("/").pop()
                glyph: !modelData.enabled ? "triangle-alert" : "boxes"
                detail: modelData.version
                where: modelData.path
                detailWidth: 14 * page.u
                statusWidth: page.statusWidth
                status: page.statusOf(modelData)
                statusStrong: attention
                expanded: page.expandedKey === pkey
                tabStop: page.tabRow === pkey
                onExpandRequested: value => page.setExpanded(pkey, value)
                onStepRequested: delta => page.step(pkey, delta)
                onShownChanged: if (page.revealKey === pkey) page.host.showAll(row, header.height + shown)
                Connections {
                    target: row.header
                    function onActiveFocusChanged() { if (row.header.activeFocus) page.tabKey = row.pkey }
                }

                // Trust... where the status is, in a slot as wide for every row, so the columns line up.
                trailing: Item {
                    width: page.trustSlot
                    height: 7 * page.u
                    KitChip {
                        objectName: "packTrust-" + row.pid
                        anchors.right: parent.right
                        look: page.look
                        visible: row.modelData.enabled && !row.modelData.trusted
                        enabled: !page.model.busy
                        activeFocusOnTab: enabled && row.tabStop
                        text: qsTr("Trust…")
                        Accessible.name: qsTr("Trust %1").arg(row.label)
                        onTapped: { page.noteFocus(); page.model.trust(row.modelData.folder) }
                    }
                }

                // Where it is, how many nodes, and what the load or the Default said about it.
                Item {
                    width: parent.width
                    height: info.implicitHeight + 3 * page.u
                    ColumnLayout {
                        id: info
                        x: page.look.params.rowPadLeft * page.u
                        y: 1.5 * page.u
                        width: parent.width - (page.look.params.rowPadLeft + page.look.params.rowPadRight) * page.u
                        spacing: page.u
                        KitBadge {
                            objectName: "packInherited-" + row.pid
                            look: page.look
                            visible: row.modelData.inherited
                            text: qsTr("Inherited from Default")
                        }
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            textFormat: Text.PlainText
                            text: qsTr("%1 · %2 · %n nodes", "", row.modelData.nodes).arg(row.modelData.version).arg(row.modelData.path)
                            color: page.look.label
                            font.pixelSize: page.look.labelSize
                        }
                        // What its programs may do (6.7.1), or that it runs as you: the lines of the Trust question.
                        Text {
                            objectName: "packNeeds-" + row.pid
                            visible: row.modelData.enabled && row.modelData.needs.length > 0
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: row.modelData.needs.join("\n")
                            color: row.modelData.sandboxed ? page.look.label : page.look.text
                            font.pixelSize: page.look.labelSize
                            font.bold: !row.modelData.sandboxed
                        }
                        // An inherited pack runs from the Default project's folder: what that means, once (PacksModel).
                        Text {
                            objectName: "packWarning-" + row.pid
                            visible: row.modelData.warning !== ""
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: row.modelData.warning
                            color: page.look.label
                            font.pixelSize: page.look.labelSize
                        }
                        Repeater {
                            model: row.modelData.problems
                            delegate: RowLayout {
                                id: problemRow
                                required property string modelData
                                Layout.fillWidth: true
                                spacing: page.u
                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    textFormat: Text.PlainText
                                    color: page.look.error
                                    font.pixelSize: page.look.labelSize
                                    text: problemRow.modelData
                                }
                                KitCopyButton {
                                    objectName: "packProblemCopy"
                                    look: page.look
                                    Layout.alignment: Qt.AlignTop
                                    copyText: problemRow.modelData
                                }
                            }
                        }
                    }
                }

                // The secrets the pack declares (6.4, #402): set or not, never the value. Typing one and pressing Enter (or
                // Set) stores it in pack-secrets.json (0600); Clear drops it. Only the pack's own nodes ever receive it.
                Repeater {
                    model: row.modelData.secrets
                    delegate: Column {
                        id: keyed
                        required property var modelData
                        readonly property string key: row.pid + "-" + modelData.name
                        width: parent ? parent.width : 0
                        KitSeparator { look: page.look; height: 0 } // drawn over the first row's top: the rows draw their own lines below
                        KitRow {
                            look: page.look
                            label: keyed.modelData.label
                            KitBadge {
                                objectName: "packSecretState-" + keyed.key
                                look: page.look
                                text: keyed.modelData.set ? qsTr("set") : qsTr("not set")
                            }
                        }
                        Item { // what the secret is for, in full, under its name
                            visible: keyed.modelData.description !== ""
                            width: parent.width
                            height: visible ? descText.implicitHeight + 2 * page.u : 0
                            Text {
                                id: descText
                                x: page.look.params.rowPadLeft * page.u
                                width: parent.width - (page.look.params.rowPadLeft + page.look.params.rowPadRight) * page.u
                                text: keyed.modelData.description
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                                color: page.look.label
                                font.pixelSize: page.look.labelSize
                            }
                        }
                        // Where the value may be sent: set with the value, then only shown (to change it, Clear and set again).
                        KitRow {
                            objectName: "packSecretOrigin-" + keyed.key
                            look: page.look
                            visible: keyed.modelData.set && keyed.modelData.origin !== ""
                            label: qsTr("Only for %1").arg(keyed.modelData.origin)
                        }
                        KitRow {
                            look: page.look
                            visible: !keyed.modelData.set
                            label: qsTr("Only for")
                            hint: keyed.modelData.originRequired ? qsTr("Required") : qsTr("Optional")
                            SettingsParts.Field {
                                implicitWidth: Math.min(60 * page.u, page.width - 16 * page.u) // never wider than the tray, narrow or wide
                                id: originField
                                objectName: "packSecretOnlyFor-" + keyed.key
                                label: qsTr("Only for")
                                placeholder: qsTr("https://api.example.com")
                                onCommitted: text => originField.value = text
                            }
                        }
                        KitRow {
                            look: page.look
                            label: qsTr("Key")
                            hint: keyed.modelData.set ? qsTr("Saved") : ""
                            Flow { // the field and its chips: the field gives up width down to 30u, then the chips go under it (#540)
                                id: keyFlow
                                readonly property real room: page.width - 16 * page.u // never wider than the tray, narrow or wide
                                readonly property real chipsWidth: setChip.implicitWidth + (clearChip.visible ? clearChip.implicitWidth + spacing : 0)
                                spacing: 2 * page.u
                                width: Math.min(secretField.implicitWidth + spacing + chipsWidth, room)
                                SettingsParts.Field {
                                    implicitWidth: Math.min(60 * page.u, Math.max(keyFlow.room - keyFlow.spacing - keyFlow.chipsWidth, 30 * page.u), keyFlow.room)
                                    id: secretField
                                    objectName: "packSecret-" + keyed.key
                                    secret: true
                                    label: qsTr("Key")
                                    placeholder: keyed.modelData.set ? qsTr("Set; type a new one to replace it") : qsTr("Type it and press Enter")
                                    onCommitted: text => {
                                        // After this handler: the list is rebuilt by the change, and this field is in it.
                                        if (text !== "") {
                                            const origin = keyed.modelData.set ? keyed.modelData.origin : originField.input.text
                                            page.noteFocus()
                                            Qt.callLater(() => page.model.setSecret(row.modelData.id, keyed.modelData.name, text, origin, row.modelData.path))
                                        }
                                    }
                                }
                                KitChip {
                                    id: setChip
                                    objectName: "packSecretSet-" + keyed.key
                                    look: page.look
                                    text: qsTr("Set…")
                                    onTapped: secretField.commit()
                                }
                                KitChip {
                                    id: clearChip
                                    objectName: "packSecretClear-" + keyed.key
                                    look: page.look
                                    visible: keyed.modelData.set
                                    text: qsTr("Clear")
                                    onTapped: { page.noteFocus(); Qt.callLater(() => page.model.clearSecret(row.modelData.id, keyed.modelData.name, row.modelData.path)) }
                                }
                            }
                        }
                    }
                }

                // The footer: Update at the start, Remove, in its own place, at the end.
                KitSeparator { look: page.look; visible: footer.visible }
                Item {
                    id: footer
                    width: parent.width
                    height: 10 * page.u
                    visible: row.modelData.removable || row.modelData.inherited
                    KitChip {
                        id: updateChip
                        objectName: "packUpdate-" + row.pid
                        anchors { left: parent.left; leftMargin: page.look.params.rowPadLeft * page.u; verticalCenter: parent.verticalCenter }
                        look: page.look
                        visible: row.modelData.removable && row.modelData.source !== ""
                        enabled: !page.model.busy
                        text: qsTr("Update")
                        Accessible.name: qsTr("Update %1").arg(row.label)
                        onTapped: page.model.update(row.modelData.folder)
                    }
                    KitChip {
                        id: removeChip
                        objectName: "packRemove-" + row.pid
                        anchors { right: parent.right; rightMargin: page.look.params.rowPadRight * page.u; verticalCenter: parent.verticalCenter }
                        look: page.look
                        danger: true
                        visible: row.modelData.removable || row.modelData.inherited
                        enabled: !page.model.busy
                        text: qsTr("Remove…")
                        Accessible.name: qsTr("Remove %1").arg(row.label)
                        onTapped: page.model.remove(row.modelData.folder)
                    }
                }
            }
        }
    }

    // The library's groups (#74): renamed here (folder and id follow) or removed after a yes.
    // Workflows keep their own copy of a group, so neither changes any of them.
    Text {
        visible: page.shownLibrary.length > 0
        Layout.fillWidth: true
        Layout.topMargin: 5 * page.u
        Layout.leftMargin: 2 * page.u
        text: qsTr("Library: groups saved from workflows. A workflow keeps its own copy of each group it uses, so renaming or removing one here changes no workflow.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: page.look.label
        font.pixelSize: page.look.labelSize
    }
    KitGroup {
        objectName: "libraryList"
        visible: page.shownLibrary.length > 0
        Layout.fillWidth: true
        Layout.topMargin: 2 * page.u
        look: page.look
        Repeater {
            model: page.model.library
            delegate: KitRow {
                id: entry
                required property var modelData
                property bool renaming: false
                property bool confirming: false
                objectName: "libraryGroup-" + modelData.id
                look: page.look
                visible: !page.searching || page.groupMatches(modelData)
                label: confirming ? qsTr("Remove %1 from the library?").arg(modelData.name) : modelData.name
                hint: confirming ? "" : modelData.version
                SettingsParts.Field {
                    implicitWidth: Math.min(60 * page.u, page.width - 16 * page.u) // never wider than the tray, narrow or wide
                    visible: entry.renaming
                    objectName: "libraryName-" + entry.modelData.id
                    label: qsTr("Rename %1").arg(entry.modelData.name)
                    value: entry.modelData.name
                    onCommitted: text => {
                        entry.renaming = false
                        page.model.renameLibraryGroup(entry.modelData.id, text)
                    }
                }
                KitChip {
                    objectName: "libraryRename-" + entry.modelData.id
                    look: page.look
                    enabled: !page.model.busy
                    visible: !entry.renaming && !entry.confirming
                    text: qsTr("Rename…")
                    onTapped: entry.renaming = true
                }
                KitChip {
                    objectName: "libraryRemove-" + entry.modelData.id
                    look: page.look
                    enabled: !page.model.busy
                    visible: !entry.renaming && !entry.confirming
                    text: qsTr("Remove…")
                    onTapped: entry.confirming = true
                }
                KitChip {
                    objectName: "libraryRemoveNo-" + entry.modelData.id
                    look: page.look
                    visible: entry.confirming
                    text: qsTr("Keep")
                    onTapped: entry.confirming = false
                }
                KitChip {
                    objectName: "libraryRemoveYes-" + entry.modelData.id
                    look: page.look
                    danger: true
                    enabled: !page.model.busy
                    visible: entry.confirming
                    text: qsTr("Remove")
                    onTapped: {
                        entry.confirming = false
                        page.model.removeLibraryGroup(entry.modelData.id)
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
            id: addRow
            objectName: "packAddRow"
            look: page.look
            label: qsTr("Add a pack")
            glyph: "plus"
            tray: false
            expanded: page.addOpen || page.model.packs.length === 0
            onExpandRequested: value => page.addOpen = value
            onStepRequested: delta => {
                if (delta < 0 && page.shownKeys.length > 0)
                    page.focusKey(page.shownKeys[page.shownKeys.length - 1])
            }
            KitRow {
                look: page.look
                label: qsTr("Folder or git address")
                hint: qsTr("A folder with a pack.json, or a git address")
                hintLines: 2
                SettingsParts.Field {
                    implicitWidth: Math.min(60 * page.u, page.width - 16 * page.u) // never wider than the tray, narrow or wide
                    id: where
                    objectName: "packLocation"
                    label: qsTr("Folder or git address")
                    placeholder: qsTr("Folder or git address")
                    onCommitted: text => where.value = text
                }
            }
            KitRow {
                look: page.look
                label: qsTr("Tag or commit")
                hint: qsTr("Git only: pins the version")
                SettingsParts.Field {
                    implicitWidth: Math.min(60 * page.u, page.width - 16 * page.u) // never wider than the tray, narrow or wide
                    id: pin
                    objectName: "packRef"
                    label: qsTr("Tag or commit")
                    placeholder: qsTr("Tag or commit")
                    onCommitted: text => pin.value = text
                }
            }
            KitRow {
                look: page.look
                label: qsTr("Add the pack")
                hint: qsTr("You are asked before anything is installed")
                hintLines: 2
                KitChip {
                    objectName: "packAdd"
                    look: page.look
                    enabled: !page.model.busy
                    text: qsTr("Add…")
                    onTapped: {
                        where.commit()
                        pin.commit()
                        page.model.add(where.value, pin.value)
                    }
                }
            }
        }
    }

    // --- what the last change said ---------------------------------------------------------------

    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: 3 * page.u
        Layout.leftMargin: 2 * page.u
        spacing: page.u
        visible: page.model.busy || page.model.message !== "" || page.model.failure !== "" || page.model.log.length > 0
        RowLayout {
            visible: page.model.busy
            spacing: 2 * page.u
            Text {
                objectName: "packsBusy"
                text: qsTr("Working…")
                color: page.look.label
                font.pixelSize: page.look.labelSize
            }
            KitChip {
                objectName: "packsCancel"
                danger: true
                look: page.look
                text: qsTr("Cancel")
                onTapped: page.model.cancel()
            }
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
        RowLayout {
            Layout.fillWidth: true
            visible: page.model.failure !== ""
            spacing: page.u
            Text {
                objectName: "packsFailure"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: page.look.error
                font.pixelSize: page.look.labelSize
                text: page.model.failure
            }
            KitCopyButton {
                objectName: "packsFailureCopy"
                look: page.look
                Layout.alignment: Qt.AlignTop
                copyText: page.model.failure
            }
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
}
