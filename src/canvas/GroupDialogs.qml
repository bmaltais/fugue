pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../kit"
import "widgets"

// What groups and Frames ask (spec 5): whether Make group leaves the selected sinks outside
// (5.5), a port's new label (#72), a library update or rename (5.3), and the interface editor of
// the group shown (5.4): its name, its exposed inputs (label, what they set, the inner inputs
// they drive, order; name, section, default, range and choices under Details) and outputs.
// Every change is one edit of App.canvasModel; a refusal shows its reason.
Item {
    id: root

    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    readonly property CanvasLook look: App.theme.look
    readonly property CanvasModel model: App.canvasModel

    property alias sinksQuestion: sinksAsk
    property alias rename: renameAsk
    property alias interfaceEditor: editor

    Connections {
        target: root.model.groups
        function onMakeGroupAsked(sinks) {
            sinksAsk.sinks = sinks
            sinksAsk.open()
        }
        function onLibraryRenameOffered(libraryId, name) {
            libraryAsk.libraryId = libraryId
            libraryAsk.name = name
            libraryAsk.open()
        }
        function onGroupUpdateAsked(node, changes) {
            updateAsk.node = node
            updateAsk.changes = changes
            updateAsk.major = false
            updateAsk.open()
        }
    }

    Connections {
        target: root.model
        function onPortRenameRequested(node, port, output) {
            const found = root.model.groups.portToRename(node, port, output)
            const prompt = output ? qsTr("Output name on the group") : qsTr("Input name on the group")
            renameAsk.ask(node, port, output, prompt, found.label || "")
        }
        // A node's newer major version (3.6, #118): the same question, said for one node.
        function onNodeMoveAsked(node, changes) {
            updateAsk.node = node
            updateAsk.changes = changes
            updateAsk.major = true
            updateAsk.open()
        }
        function onInterfaceEditorRequested() {
            if (root.model.inGroup)
                editor.open()
        }
    }

    // Why a group edit was refused (when the editor is not there to say it), or where a group
    // was saved.
    Rectangle {
        objectName: "groupNotice"
        visible: (root.model.groups.problem !== "" && !editor.visible) || root.model.groups.notice !== ""
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: visible ? noticeRow.implicitHeight + 4 * root.look.unit : 0
        color: root.look.popup
        border.color: root.look.hairline
        RowLayout {
            id: noticeRow
            anchors { fill: parent; margins: 2 * root.look.unit }
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: root.look.text
                text: root.model.groups.problem !== "" ? root.model.groups.problem : root.model.groups.notice
            }
            Loader { // made only while a problem shows
                Layout.alignment: Qt.AlignTop
                active: root.model.groups.problem !== ""
                sourceComponent: KitCopyButton {
                    objectName: "groupBannerCopy"
                    look: root.look
                    copyText: root.model.groups.problem
                }
            }
            ViewButton {
                look: root.look
                flat: true
                compact: true
                glyph: "x"
                tip: qsTr("Dismiss")
                activeFocusOnTab: false
                onTapped: root.model.groups.dismiss()
            }
        }
    }

    Question {
        id: sinksAsk
        objectName: "groupSinksAsk"
        property var sinks: []
        ColumnLayout {
            anchors.fill: parent
            spacing: 4 * root.look.unit
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("A group never writes files or shows previews from inside. Leave %1 outside the group, wired to its output?")
                          .arg(sinksAsk.sinks.join(", "))
            }
            KitDialogButtons {
                ThemedButton { dialogRole: "cancel"; text: qsTr("Cancel"); onClicked: sinksAsk.close() }
                ThemedButton {
                    objectName: "groupSinksInside"
                    text: qsTr("Put inside")
                    onClicked: { sinksAsk.close(); root.model.groups.makeGroup(false) }
                }
                ThemedButton {
                    objectName: "groupSinksOutside"
                    text: qsTr("Leave outside")
                    highlighted: true
                    onClicked: { sinksAsk.close(); root.model.groups.makeGroup(true) }
                }
            }
        }
    }

    // An update from the library (5.3) says what it changes first (3.6); every instance moves.
    // The same question moves one node to a newer major of its type (#118).
    Question {
        id: updateAsk
        objectName: "groupUpdateAsk"
        property string node: ""
        property var changes: ({})
        property bool major: false // one node's newer major, not a group's library update
        function line(label, names) { return names && names.length > 0 ? label.arg(names.join(", ")) : "" }
        ColumnLayout {
            anchors.fill: parent
            spacing: 3 * root.look.unit
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.bold: true
                text: (updateAsk.major ? qsTr("Move %1 from %2 to %3?") : qsTr("Update %1 from %2 to %3?"))
                          .arg(updateAsk.changes.name || "")
                          .arg(updateAsk.changes.from || "").arg(updateAsk.changes.to || "")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: [updateAsk.line(qsTr("New inputs: %1"), updateAsk.changes.addedInputs),
                       updateAsk.line(qsTr("Inputs that go: %1"), updateAsk.changes.removedInputs),
                       updateAsk.line(qsTr("New outputs: %1"), updateAsk.changes.addedOutputs),
                       updateAsk.line(qsTr("Outputs that go: %1"), updateAsk.changes.removedOutputs)]
                          .filter(l => l !== "").join("\n")
                      || qsTr("The inputs and outputs stay the same.")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: root.look.label
                text: updateAsk.major
                      ? qsTr("The node keeps its values and links, and runs on the new version. Inputs and outputs that go are flagged where they no longer fit; undo moves it back.")
                      : qsTr("Every instance of the group in this workflow moves to the new version; values and links of inputs and outputs that go are dropped.")
            }
            KitDialogButtons {
                ThemedButton { dialogRole: "cancel"; text: qsTr("Cancel"); onClicked: updateAsk.close() }
                ThemedButton {
                    objectName: "groupUpdateOk"
                    text: updateAsk.major ? qsTr("Move") : qsTr("Update")
                    highlighted: true
                    onClicked: {
                        updateAsk.close()
                        if (updateAsk.major)
                            root.model.moveToNewerMajor(updateAsk.node)
                        else
                            root.model.groups.updateGroupInstance(updateAsk.node)
                    }
                }
            }
        }
    }

    // A group of the library was renamed in this workflow: rename it there too (#74)? Its folder
    // and id follow; other workflows keep their copy and lose their link to it.
    Question {
        id: libraryAsk
        objectName: "libraryRenameAsk"
        property string libraryId: ""
        property string name: ""
        ColumnLayout {
            anchors.fill: parent
            spacing: 4 * root.look.unit
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("%1 is in the library. Rename it there too, as \u201c%2\u201d?").arg(libraryAsk.libraryId, libraryAsk.name)
            }
            KitDialogButtons {
                ThemedButton { dialogRole: "cancel"; text: qsTr("Only here"); onClicked: libraryAsk.close() }
                ThemedButton {
                    objectName: "libraryRenameYes"
                    text: qsTr("Rename in the library")
                    highlighted: true
                    onClicked: {
                        libraryAsk.close()
                        root.model.groups.renameLibraryGroup(libraryAsk.libraryId, libraryAsk.name)
                    }
                }
            }
        }
    }

    // Rename a port on a group's face (#72): its field filled with the label shown now, selected;
    // empty goes back to the inner port's. (Node and Frame titles are edited in place, #74.)
    Question {
        id: renameAsk
        objectName: "renameAsk"
        property string target: ""
        property string port: ""
        property bool output: false
        property string prompt: ""
        function ask(target, port, output, prompt, current) {
            renameAsk.target = target
            renameAsk.port = port
            renameAsk.output = output
            renameAsk.prompt = prompt
            renameField.text = current
            renameAsk.open()
        }
        onOpened: {
            renameField.forceActiveFocus()
            renameField.selectAll()
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 3 * root.look.unit
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label { text: renameAsk.prompt }
            ThemedTextField {
                id: renameField
                objectName: "renameField"
                Layout.fillWidth: true
                onAccepted: if (renameOk.enabled) renameOk.clicked()
            }
            KitDialogButtons {
                ThemedButton { dialogRole: "cancel"; text: qsTr("Cancel"); onClicked: renameAsk.close() }
                ThemedButton {
                    id: renameOk
                    objectName: "renameOk"
                    text: qsTr("Rename")
                    highlighted: true
                    onClicked: {
                        renameAsk.close()
                        root.model.groups.renamePort(renameAsk.target, renameAsk.port, renameAsk.output, renameField.text)
                    }
                }
            }
        }
    }

    // The interface editor (5.4): what the group's card shows. A dusk header names it and says what it is for; the name, then the
    // inputs and the outputs as kit groups, each row its label on the card, what it sets or where it comes from in words, and its
    // order and removal; the rest (the name links use, section, default, range, choices, the type) under its Details. The lists
    // under each section add inner ports, as Expose on the group does from a port. Every change is one edit at once; the rows
    // are made from groupInterface() again after each, and the focus stays on the control it was on (the row's, after a move).
    // Keys: Tab walks it in reading order; Up and Down in a label go to the row above or below, Alt+Up and Alt+Down move the
    // row; Enter keeps a field's text; Escape closes.
    Question {
        id: editor
        objectName: "groupInterfaceEditor"
        heading: qsTr("Group inputs and outputs")
        subheading: qsTr("Choose what this group shows on its card: the inputs it asks for and the results it gives.")
        tone: "dusk"
        // Centred when it opens; then its top stays where it is, so a row under the pointer does not move when Details open or a
        // row is added: it grows downwards, and up only when it would leave the window.
        parent: Overlay.overlay
        anchors.centerIn: undefined
        property real openedTop: -1 // where its top was once it had opened and was laid out; -1 while centred
        readonly property real centredTop: ((parent ? parent.height : 0) - height) / 2
        x: Math.round(((parent ? parent.width : 0) - width) / 2)
        y: Math.round(openedTop < 0 ? centredTop
                      : Math.max(6 * App.theme.look.unit, Math.min(openedTop, (parent ? parent.height : 0) - height - 6 * App.theme.look.unit)))
        onClosed: openedTop = -1
        width: Math.min((Overlay.overlay ? Overlay.overlay.width : 760) - 12 * App.theme.look.unit, 720)
        // As tall as what it holds (a one-row group is a short dialog), up to the window less its margins or 720px; the list scrolls
        // past that.
        height: Math.min((Overlay.overlay ? Overlay.overlay.height : 680) - 12 * App.theme.look.unit, 720,
                         topPadding + bottomPadding + editorBody.implicitHeight)
        property var face: ({})
        property var details: ({}) // the rows whose Details are open, by "in:<name>" or "out:<name>"; kept across reloads
        property string focusAfter: "" // the control to focus once the rows are made again (a moved row's)
        property bool holding: false // an action runs: the rows are made again once, after it
        property bool reloadWanted: false
        property bool reloading: false // the rows are being made again: a field that goes now keeps nothing
        function reload() {
            if (holding) {
                reloadWanted = true
                return
            }
            const focused = editor.contentItem.Window.activeFocusItem
            const name = focusAfter !== "" ? focusAfter : (focused && inside(focused) ? focused.objectName : "")
            focusAfter = ""
            reloading = true
            face = root.model.groups.groupInterface()
            reloading = false
            if (name !== "")
                Qt.callLater(refocus, name)
        }
        // A range's field: empty (no limit) or a typed number (TypedNumber: a comma is a decimal point only where the language
        // writes one); `number` gives it with a point, as the range is stored and shown.
        readonly property RegularExpressionValidator rangeValidator: RegularExpressionValidator {
            regularExpression: TypedNumber.pattern
        }
        function number(text) { return TypedNumber.normalize(text) }
        // Whether a field's text is a change to keep: not while the rows are made again (a field that goes then would keep its
        // text a second time, against the row it showed).
        function keeps(text, current) {
            return !reloading && text !== (current === undefined || current === null ? "" : String(current))
        }
        // Runs an edit; the rows are made again once, after it. A row's button or key (`commit`) first keeps what the focused
        // field holds: a button takes no focus, and Alt+Down moves the row with the cursor still in the field.
        function act(action, commit) {
            if (holding)
                return action()
            holding = true
            let done = false
            try {
                if (commit) {
                    const item = editor.contentItem.Window.activeFocusItem
                    // Only what the field would keep on its own: a half-typed number ("-", "1e") is not saved as no limit.
                    if (item && inside(item) && item instanceof ThemedTextField && item.acceptableInput)
                        item.editingFinished()
                }
                done = action()
            } finally {
                holding = false
            }
            if (reloadWanted) {
                reloadWanted = false
                reload()
            }
            return done
        }
        function inside(item) {
            for (let at = item; at; at = at.parent)
                if (at === editor.contentItem)
                    return true
            return false
        }
        function find(from, name) {
            for (const child of from.children) {
                if (child.objectName === name && child.visible)
                    return child
                const found = find(child, name)
                if (found)
                    return found
            }
            return null
        }
        function refocus(name) {
            const item = find(editor.contentItem, name)
            if (item)
                item.forceActiveFocus()
            else if (!inside(editor.contentItem.Window.activeFocusItem))
                nameField.forceActiveFocus() // the control went with its row: the focus stays in the editor
        }
        function toggleDetails(key) {
            const next = Object.assign({}, details)
            next[key] = !next[key]
            details = next
        }
        function renameDetails(from, to) {
            if (!details[from])
                return
            const next = Object.assign({}, details)
            delete next[from]
            next[to] = true
            details = next
        }
        // Moves a row and keeps the focus on the moved row's `control` ("Label", "Up" or "Down"); a button that cannot act at the new
        // place (Move up on the first row) hands it to the other one.
        // The row at `index` as the group has it now: after the focused field was kept, which may have renamed it (a Name typed
        // under Details), so an action never uses a name the row no longer has.
        function rowNow(output, index) {
            const now = root.model.groups.groupInterface()
            return ((output ? now.outputs : now.inputs) || [])[index]
        }
        function move(output, index, to, control) {
            const count = ((output ? face.outputs : face.inputs) || []).length
            if (to < 0 || to >= count)
                return
            const shown = control === "Up" && to === 0 ? "Down" : control === "Down" && to === count - 1 ? "Up" : control
            act(() => {
                const row = rowNow(output, index)
                focusAfter = (output ? "exposedOutput" : "exposed") + shown + to
                if (!row || !root.model.groups.moveExposed(output, row.name, to))
                    focusAfter = ""
            }, true)
        }
        // Stops exposing a row; the focus goes to the same button of the row that takes its place, else of the row above, else
        // to the section's Add button.
        function remove(output, index) {
            const rows = (output ? face.outputs : face.inputs) || []
            const prefix = output ? "exposedOutputRemove" : "exposedRemove"
            act(() => {
                const row = rowNow(output, index)
                focusAfter = index + 1 < rows.length ? prefix + index : index > 0 ? prefix + (index - 1)
                           : output ? "groupAddOutput" : "groupAddInput"
                const done = row && (output ? root.model.groups.unexposeOutput(row.name) : root.model.groups.removeExposedInput(row.name))
                if (!done)
                    focusAfter = ""
            }, true)
        }
        onOpened: {
            details = ({}) // each opening starts with every row's Details shut
            reload()
            nameField.forceActiveFocus()
            Qt.callLater(() => { if (editor.visible) editor.openedTop = editor.centredTop })
        }
        Connections {
            target: root.model
            enabled: editor.visible
            function onEdited() { editor.reload() }
            function onScopeChanged() { if (!root.model.inGroup) editor.close(); else editor.reload() }
        }

        // A quiet line under a section or a row.
        component Quiet: Text {
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: root.look.label
            font.pixelSize: root.look.labelSize
        }
        // One setting of a row's Details: its name, then the field.
        component DetailLabel: Text {
            textFormat: Text.PlainText
            color: root.look.label
            font.pixelSize: root.look.labelSize
            Layout.alignment: Qt.AlignVCenter
        }
        component RowButton: ViewButton {
            look: root.look
            flat: true
            compact: true
        }
        // An action that adds one of a list (Add an input…, Also set…): a button with a plus, opening the window's menu of the
        // choices ({title}; arrows move, Enter picks, typing filters a long list, Escape closes). `chosen(index)` says which.
        component AddButton: ViewButton {
            id: add
            property var choices: []
            signal chosen(int index)
            look: root.look
            glyph: "plus"
            trailingGlyph: "chevron-down"
            visible: choices.length > 0
            onTapped: {
                const at = add.mapToItem(Overlay.overlay, 0, add.height)
                // Right to left the menu's end meets the button's end.
                addMenu.wantX = root.rightToLeft ? at.x + add.width - addMenu.width : at.x
                addMenu.wantY = at.y
                addMenu.anchorHeight = add.height
                addMenu.open()
            }
            WidgetMenu {
                id: addMenu
                objectName: "groupAddMenu" // not "widgetMenu": that is the canvas's own, which tests find by name
                look: root.look
                parent: Overlay.overlay
                anchorWidth: add.width
                searchable: add.choices.length > 12
                entries: add.choices.map((c, i) => ({text: c.title, value: i}))
                onPicked: v => {
                    add.forceActiveFocus()
                    add.chosen(Number(v))
                }
            }
        }

        // One exposed input.
        component InputRow: Item {
            id: inputRow
            required property var modelData
            required property int index
            readonly property string key: "in:" + modelData.name
            readonly property bool open: editor.details[key] === true
            readonly property int count: (editor.face.inputs || []).length
            width: parent ? parent.width : implicitWidth
            implicitHeight: column.implicitHeight + 3 * root.look.unit
            height: implicitHeight
            // A value as its field shows it: empty when there is none.
            function shown(key) {
                const value = modelData[key]
                return value === undefined || value === null ? "" : String(value)
            }
            function change(key, value) {
                const next = {name: modelData.name, label: modelData.label, section: modelData.section,
                              min: modelData.min, max: modelData.max, type: modelData.type,
                              default: shown("default"), choices: modelData.choices}
                next[key] = value
                const from = inputRow.key
                const index = inputRow.index
                editor.act(() => {
                    // A new name re-keys the open Details only once it is taken; a refused one leaves the field as typed.
                    if (root.model.groups.editExposedInput(modelData.name, next) && key === "name") {
                        editor.renameDetails(from, "in:" + value.trim())
                        editor.focusAfter = "exposedName" + index
                    }
                }, false)
            }
            // Up and Down go to the label above or below; with Alt they move the row.
            function rowKeys(event) {
                if (event.key !== Qt.Key_Up && event.key !== Qt.Key_Down)
                    return
                const step = event.key === Qt.Key_Up ? -1 : 1
                if (event.modifiers & Qt.AltModifier)
                    editor.move(false, index, index + step, "Label")
                else
                    editor.refocus("exposedLabel" + (index + step))
                event.accepted = true
            }
            KitSeparator { look: root.look; visible: inputRow.index > 0 }
            ColumnLayout {
                id: column
                x: 3 * root.look.unit
                y: 1.5 * root.look.unit
                width: parent.width - 6 * root.look.unit
                spacing: root.look.unit
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2 * root.look.unit
                    ThemedTextField {
                        objectName: "exposedLabel" + inputRow.index
                        Layout.fillWidth: true
                        text: inputRow.modelData.label
                        placeholderText: inputRow.modelData.shown
                        Accessible.name: qsTr("Label on the card")
                        ToolTip.text: qsTr("The name on the group's card; empty shows %1").arg(inputRow.modelData.shown)
                        ToolTip.visible: hovered && !activeFocus
                        ToolTip.delay: root.look.tooltipDelay
                        Keys.onPressed: event => inputRow.rowKeys(event)
                        onEditingFinished: if (editor.keeps(text, inputRow.modelData.label)) inputRow.change("label", text)
                    }
                    RowButton {
                        objectName: "exposedDetails" + inputRow.index
                        text: qsTr("Details")
                        trailingGlyph: inputRow.open ? "chevron-up" : "chevron-down"
                        tip: qsTr("Name, section, default, range, choices and type")
                        checkable: true
                        current: inputRow.open
                        accented: false
                        onTapped: editor.toggleDetails(inputRow.key)
                    }
                    RowButton {
                        objectName: "exposedUp" + inputRow.index
                        glyph: "chevron-up"
                        tip: qsTr("Move up (Alt+Up)")
                        enabled: inputRow.index > 0
                        onTapped: editor.move(false, inputRow.index, inputRow.index - 1, "Up")
                    }
                    RowButton {
                        objectName: "exposedDown" + inputRow.index
                        glyph: "chevron-down"
                        tip: qsTr("Move down (Alt+Down)")
                        enabled: inputRow.index < inputRow.count - 1
                        onTapped: editor.move(false, inputRow.index, inputRow.index + 1, "Down")
                    }
                    RowButton {
                        objectName: "exposedRemove" + inputRow.index
                        glyph: "x"
                        tip: qsTr("Stop exposing")
                        onTapped: editor.remove(false, inputRow.index)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2 * root.look.unit
                    Quiet {
                        objectName: "exposedSets" + inputRow.index
                        Layout.fillWidth: true
                        text: inputRow.modelData.sets
                    }
                    AddButton {
                        objectName: "exposedAlsoDrive" + inputRow.index
                        flat: true
                        compact: true
                        text: qsTr("Also set…")
                        tip: qsTr("One input of the card that sets another inner input of the same type too")
                        choices: editor.visible ? root.model.groups.exposeCandidates(inputRow.modelData.name) : []
                        onChosen: i => {
                            const picked = choices[i]
                            const index = inputRow.index
                            editor.act(() => {
                                const row = editor.rowNow(false, index)
                                if (row)
                                    root.model.groups.exposeInput(picked.node, picked.input, row.name)
                            }, true)
                        }
                    }
                }
                Loader { // the Details, made only while open
                    Layout.fillWidth: true
                    active: inputRow.open
                    visible: active
                    sourceComponent: GridLayout {
                        columns: 2
                        columnSpacing: 2 * root.look.unit
                        rowSpacing: root.look.unit
                        DetailLabel { text: qsTr("Name") }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 2 * root.look.unit
                            ThemedTextField {
                                objectName: "exposedName" + inputRow.index
                                Layout.preferredWidth: 35 * root.look.unit
                                text: inputRow.modelData.name
                                Accessible.name: qsTr("Name")
                                onEditingFinished: if (editor.keeps(text, inputRow.modelData.name)) inputRow.change("name", text)
                            }
                            Quiet {
                                Layout.fillWidth: true
                                text: qsTr("What links and the command line use")
                            }
                        }
                        DetailLabel { text: qsTr("Section") }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 2 * root.look.unit
                            ThemedTextField {
                                objectName: "exposedSection" + inputRow.index
                                Layout.preferredWidth: 35 * root.look.unit
                                text: inputRow.modelData.section
                                placeholderText: qsTr("None")
                                Accessible.name: qsTr("Section")
                                onEditingFinished: if (editor.keeps(text, inputRow.modelData.section)) inputRow.change("section", text)
                            }
                            Quiet {
                                Layout.fillWidth: true
                                text: qsTr("Advanced folds on the card")
                            }
                        }
                        DetailLabel { text: qsTr("Default") }
                        ThemedTextField {
                            objectName: "exposedDefault" + inputRow.index
                            Layout.preferredWidth: 35 * root.look.unit
                            text: inputRow.shown("default")
                            placeholderText: qsTr("The inner value")
                            Accessible.name: qsTr("Default")
                            onEditingFinished: if (editor.keeps(text, inputRow.shown("default"))) inputRow.change("default", text)
                        }
                        DetailLabel {
                            visible: inputRow.modelData.type === "number"
                            text: qsTr("Range")
                        }
                        RowLayout {
                            visible: inputRow.modelData.type === "number"
                            spacing: 2 * root.look.unit
                            ThemedTextField {
                                objectName: "exposedMin" + inputRow.index
                                Layout.preferredWidth: 20 * root.look.unit
                                text: inputRow.shown("min")
                                placeholderText: qsTr("min")
                                Accessible.name: qsTr("Smallest value")
                                validator: editor.rangeValidator
                                onEditingFinished: if (editor.keeps(editor.number(text), inputRow.shown("min"))) inputRow.change("min", editor.number(text))
                            }
                            ThemedTextField {
                                objectName: "exposedMax" + inputRow.index
                                Layout.preferredWidth: 20 * root.look.unit
                                text: inputRow.shown("max")
                                placeholderText: qsTr("max")
                                Accessible.name: qsTr("Largest value")
                                validator: editor.rangeValidator
                                onEditingFinished: if (editor.keeps(editor.number(text), inputRow.shown("max"))) inputRow.change("max", editor.number(text))
                            }
                        }
                        DetailLabel {
                            visible: choicesField.visible
                            text: qsTr("Choices")
                        }
                        ThemedTextField {
                            id: choicesField
                            objectName: "exposedChoices" + inputRow.index
                            visible: inputRow.modelData.choices !== "" || inputRow.modelData.innerChoices !== ""
                            Layout.fillWidth: true
                            text: inputRow.modelData.choices
                            placeholderText: inputRow.modelData.innerChoices
                            Accessible.name: qsTr("Choices")
                            onEditingFinished: if (editor.keeps(text, inputRow.modelData.choices)) inputRow.change("choices", text)
                        }
                        DetailLabel { text: qsTr("Type") }
                        Quiet {
                            Layout.fillWidth: true
                            text: inputRow.modelData.type
                        }
                    }
                }
            }
        }

        // One exposed output.
        component OutputRow: Item {
            id: outputRow
            required property var modelData
            required property int index
            readonly property string key: "out:" + modelData.name
            readonly property bool open: editor.details[key] === true
            readonly property int count: (editor.face.outputs || []).length
            width: parent ? parent.width : implicitWidth
            implicitHeight: column.implicitHeight + 3 * root.look.unit
            height: implicitHeight
            // Up and Down go to the label above or below; with Alt they move the row.
            function rowKeys(event) {
                if (event.key !== Qt.Key_Up && event.key !== Qt.Key_Down)
                    return
                const step = event.key === Qt.Key_Up ? -1 : 1
                if (event.modifiers & Qt.AltModifier)
                    editor.move(true, index, index + step, "Label")
                else
                    editor.refocus("exposedOutputLabel" + (index + step))
                event.accepted = true
            }
            KitSeparator { look: root.look; visible: outputRow.index > 0 }
            ColumnLayout {
                id: column
                x: 3 * root.look.unit
                y: 1.5 * root.look.unit
                width: parent.width - 6 * root.look.unit
                spacing: root.look.unit
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2 * root.look.unit
                    ThemedTextField {
                        objectName: "exposedOutputLabel" + outputRow.index
                        Layout.fillWidth: true
                        text: outputRow.modelData.label
                        placeholderText: outputRow.modelData.shown
                        Accessible.name: qsTr("Label on the card")
                        ToolTip.text: qsTr("The name on the group's card; empty shows %1").arg(outputRow.modelData.shown)
                        ToolTip.visible: hovered && !activeFocus
                        ToolTip.delay: root.look.tooltipDelay
                        Keys.onPressed: event => outputRow.rowKeys(event)
                        onEditingFinished: if (editor.keeps(text, outputRow.modelData.label))
                                               editor.act(() => root.model.groups.relabelExposedOutput(outputRow.modelData.name, text), false)
                    }
                    RowButton {
                        objectName: "exposedOutputDetails" + outputRow.index
                        text: qsTr("Details")
                        trailingGlyph: outputRow.open ? "chevron-up" : "chevron-down"
                        tip: qsTr("Name and type")
                        checkable: true
                        current: outputRow.open
                        accented: false
                        onTapped: editor.toggleDetails(outputRow.key)
                    }
                    RowButton {
                        objectName: "exposedOutputUp" + outputRow.index
                        glyph: "chevron-up"
                        tip: qsTr("Move up (Alt+Up)")
                        enabled: outputRow.index > 0
                        onTapped: editor.move(true, outputRow.index, outputRow.index - 1, "Up")
                    }
                    RowButton {
                        objectName: "exposedOutputDown" + outputRow.index
                        glyph: "chevron-down"
                        tip: qsTr("Move down (Alt+Down)")
                        enabled: outputRow.index < outputRow.count - 1
                        onTapped: editor.move(true, outputRow.index, outputRow.index + 1, "Down")
                    }
                    RowButton {
                        objectName: "exposedOutputRemove" + outputRow.index
                        glyph: "x"
                        tip: qsTr("Stop exposing")
                        onTapped: editor.remove(true, outputRow.index)
                    }
                }
                Quiet {
                    objectName: "exposedFrom" + outputRow.index
                    Layout.fillWidth: true
                    text: outputRow.modelData.comesFrom
                }
                Loader {
                    Layout.fillWidth: true
                    active: outputRow.open
                    visible: active
                    sourceComponent: GridLayout {
                        columns: 2
                        columnSpacing: 2 * root.look.unit
                        rowSpacing: root.look.unit
                        DetailLabel { text: qsTr("Name") }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 2 * root.look.unit
                            ThemedTextField {
                                objectName: "exposedOutputName" + outputRow.index
                                Layout.preferredWidth: 35 * root.look.unit
                                text: outputRow.modelData.name
                                Accessible.name: qsTr("Name")
                                onEditingFinished: if (editor.keeps(text, outputRow.modelData.name)) {
                                    const from = outputRow.key
                                    const index = outputRow.index
                                    const name = outputRow.modelData.name
                                    const typed = text
                                    editor.act(() => {
                                        if (root.model.groups.renameExposedOutput(name, typed)) {
                                            editor.renameDetails(from, "out:" + typed.trim())
                                            editor.focusAfter = "exposedOutputName" + index
                                        }
                                    }, false)
                                }
                            }
                            Quiet {
                                Layout.fillWidth: true
                                text: qsTr("What links and the command line use")
                            }
                        }
                        DetailLabel { text: qsTr("Type") }
                        Quiet {
                            Layout.fillWidth: true
                            text: outputRow.modelData.type
                        }
                    }
                }
            }
        }

        ColumnLayout {
            id: editorBody
            anchors.fill: parent
            spacing: 3 * root.look.unit

            ScrollView {
                id: faceScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                ScrollBar.vertical: FugueScrollBar { look: root.look }
                Column {
                    width: faceScroll.availableWidth - (faceScroll.ScrollBar.vertical as FugueScrollBar).inset
                    KitGroup {
                        look: root.look
                        KitRow {
                            look: root.look
                            label: qsTr("Name")
                            hint: qsTr("The title on the group's card")
                            ThemedTextField {
                                id: nameField
                                objectName: "groupName"
                                width: 44 * root.look.unit
                                text: editor.face.name || ""
                                Accessible.name: qsTr("Group name")
                                Accessible.description: tipText
                                readonly property string tipText: qsTr("Saved as %1, version %2").arg(editor.face.id || "").arg(editor.face.version || "")
                                ToolTip.text: tipText
                                ToolTip.visible: hovered && !activeFocus
                                ToolTip.delay: root.look.tooltipDelay
                                onEditingFinished: if (editor.keeps(text, editor.face.name)) editor.act(() => root.model.groups.renameGroup(text), false)
                            }
                        }
                    }

                    RowLayout {
                        width: parent.width
                        KitSection {
                            look: root.look
                            Layout.fillWidth: true
                            text: qsTr("Inputs")
                        }
                        AddButton {
                            objectName: "groupAddInput"
                            Layout.alignment: Qt.AlignBottom
                            text: qsTr("Add an input…")
                            tip: qsTr("Show one more inner input on the card")
                            choices: editor.visible && editor.face ? root.model.groups.exposeChoices(false) : [] // made again with each reload
                            onChosen: i => editor.act(() => root.model.groups.exposeInput(choices[i].node, choices[i].port), true)
                        }
                    }
                    Item { width: 1; height: root.look.unit }
                    Quiet {
                        objectName: "groupNoInputs"
                        visible: (editor.face.inputs || []).length === 0
                        width: parent.width
                        leftPadding: 2 * root.look.unit
                        text: qsTr("No inputs yet: the card asks for nothing. Use Add an input…, or right-click an input inside the group and choose Expose on the group.")
                    }
                    KitGroup {
                        look: root.look
                        visible: (editor.face.inputs || []).length > 0
                        Repeater {
                            model: editor.face.inputs || []
                            delegate: InputRow {}
                        }
                    }

                    RowLayout {
                        width: parent.width
                        KitSection {
                            look: root.look
                            Layout.fillWidth: true
                            text: qsTr("Outputs")
                        }
                        AddButton {
                            objectName: "groupAddOutput"
                            Layout.alignment: Qt.AlignBottom
                            text: qsTr("Add an output…")
                            tip: qsTr("Show one more inner output on the card")
                            choices: editor.visible && editor.face ? root.model.groups.exposeChoices(true) : [] // made again with each reload
                            onChosen: i => editor.act(() => root.model.groups.exposeOutput(choices[i].node, choices[i].port), true)
                        }
                    }
                    Item { width: 1; height: root.look.unit }
                    Quiet {
                        objectName: "groupNoOutputs"
                        visible: (editor.face.outputs || []).length === 0
                        width: parent.width
                        leftPadding: 2 * root.look.unit
                        text: qsTr("No outputs yet: nothing leaves the group. Use Add an output…, or right-click an output inside the group and choose Expose on the group.")
                    }
                    KitGroup {
                        look: root.look
                        visible: (editor.face.outputs || []).length > 0
                        Repeater {
                            model: editor.face.outputs || []
                            delegate: OutputRow {}
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.model.groups.problem !== ""
                spacing: 2 * root.look.unit
                Text {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.look.error
                    font.pixelSize: root.look.labelSize
                    text: root.model.groups.problem
                }
                KitCopyButton {
                    objectName: "groupProblemCopy"
                    look: root.look
                    Layout.alignment: Qt.AlignTop
                    copyText: root.model.groups.problem
                }
            }

            KitDialogButtons {
                ThemedButton {
                    objectName: "groupInterfaceClose"
                    text: qsTr("Done")
                    highlighted: true
                    onClicked: editor.close()
                }
            }
        }
    }
}
