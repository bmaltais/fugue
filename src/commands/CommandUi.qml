pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "canvas"

// Everything the command registry shows or listens to in the window (spec 12.4, 12.5): one
// shortcut for each key that has an owner, the `?` overlay, the unsaved-work question on
// quit, and the notice when the keymap file could not be used. Reads App.commands and
// App.theme.
Item {
    id: root
    objectName: "commandUi"

    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    property alias overlay: help
    property alias quitDialog: quitAsk

    // A text field that has focus owns bare-letter and Shift-only keys.
    Binding {
        target: App.commands
        property: "textFieldFocus"
        value: root.Window.window ? App.commands.isTextInput(root.Window.window.activeFocusItem) : false
    }

    // One shortcut for each key that has an owner. Qt Quick turns a window's shortcuts off
    // while a modal popup is open, so each popup carries its own set (`?` closes the overlay
    // it opened; Ctrl+Q quits from the question), and the window's set sleeps meanwhile so
    // that a key never fires twice.
    component KeyShortcuts: Instantiator {
        id: keys
        property bool awake: true
        model: App.commands.activeKeys
        delegate: Shortcut {
            required property string modelData
            sequence: modelData
            enabled: keys.awake && !(App.commands.textFieldFocus && App.commands.belongsToTextField(modelData))
            onActivated: App.commands.triggerKey(modelData)
        }
    }

    // While a menu is open its rows and mnemonics own the keys (#50).
    KeyShortcuts { awake: !help.visible && !quitAsk.visible && !App.commands.menuOpen }

    Connections {
        target: App.commands
        function onShortcutsToggleRequested() { help.opened ? help.close() : help.open() }
        function onQuitConfirmationRequested() { quitAsk.open() }
    }

    // The keymap file could not be used (or holds mistakes): the defaults still work.
    Rectangle {
        id: notice
        objectName: "keymapNotice"
        property bool dismissed: false
        visible: App.commands.problems.length > 0 && !dismissed
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: visible ? noticeRow.implicitHeight + 16 : 0
        color: root.look.errorChip
        RowLayout {
            id: noticeRow
            anchors { fill: parent; margins: 8 }
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: root.look.text
                text: qsTr("Your keymap file has problems; the default keys apply where it could not be used. Press ? to see them.")
            }
            ToolButton {
                text: "×"
                focusPolicy: Qt.NoFocus
                Accessible.name: qsTr("Dismiss")
                onClicked: notice.dismissed = true
            }
        }
    }

    Popup {
        id: quitAsk
        objectName: "quitAsk"
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        padding: 6 * root.u
        Overlay.modal: Rectangle { color: root.look.scrim }
        background: PopupSurface { look: root.look }
        ColumnLayout {
            spacing: 4 * root.u
            KeyShortcuts { awake: quitAsk.opened }
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                text: qsTr("There is unsaved work. Quit anyway?")
                color: root.look.text
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button { text: qsTr("Keep working"); flat: true; onClicked: quitAsk.close() }
                Button {
                    objectName: "quitAnyway"
                    text: qsTr("Quit")
                    highlighted: true
                    onClicked: App.commands.quitNow()
                }
            }
        }
    }

    // The `?` overlay (spec 12.4, 12.5, docs/canvas-look.md 13): every command by group, in
    // two columns, with its keys as keycaps and a badge where it differs from ComfyUI.
    Popup {
        id: help
        objectName: "help"
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        topPadding: 6 * root.u
        leftPadding: 7 * root.u
        rightPadding: 7 * root.u
        bottomPadding: 5 * root.u
        width: Math.min(880, root.width - 12 * root.u)
        height: Math.min(root.height - 12 * root.u, implicitHeight)
        Overlay.modal: Rectangle { color: root.look.scrim }
        background: PopupSurface { look: root.look }
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
            NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; to: 0; duration: 90; easing.type: Easing.OutCubic }
        }
        // The registry is read each time the overlay opens, so a later registration or a
        // changed language shows.
        property var entries: []
        property string filter: ""
        onAboutToShow: {
            entries = App.commands.list()
            filter = ""
            filterField.text = ""
        }

        // The groups by command id prefix, left column then right; and Mouse, the gestures.
        readonly property var columns: [[{prefixes: ["run"], title: qsTr("Run")},
                                         {prefixes: ["node"], title: qsTr("Node")},
                                         {prefixes: ["frame", "group"], title: qsTr("Frame and group")},
                                         {gestures: true, title: qsTr("Mouse")}],
                                        [{prefixes: ["file", "app.quit", "app.settings"], title: qsTr("File")},
                                         {prefixes: ["edit"], title: qsTr("Edit")},
                                         {prefixes: ["view", "app.shortcuts", "app.menu-bar", "app.context-menu"],
                                          title: qsTr("View")},
                                         {prefixes: ["panel"], title: qsTr("Panels")}]]
        function rowsOf(group) {
            const f = filter.toLowerCase()
            const rows = []
            for (const e of entries) {
                const keys = group.gestures ? e.gestures : e.keys
                if (group.gestures ? e.gestures.length === 0
                                   : !group.prefixes.some(p => e.id === p || e.id.startsWith(p + ".")))
                    continue
                // Shortcuts only: a command with no key (a color, an export) is in the menus,
                // and shows here once the keymap file binds it (#50).
                if (!group.gestures && e.keys.length === 0)
                    continue
                for (const key of group.gestures ? keys : [keys.length > 0 ? keys[0] : ""]) {
                    const row = {description: e.description, key: key, allKeys: group.gestures ? [key] : keys,
                                 badge: !group.gestures && e.deviates ? e.versus : "", available: e.available,
                                 gesture: !!group.gestures}
                    if (f === "" || row.description.toLowerCase().indexOf(f) >= 0
                            || row.allKeys.join(" ").toLowerCase().indexOf(f) >= 0)
                        rows.push(row)
                }
            }
            return rows
        }
        // "Ctrl+Shift+Return" as the keycaps Ctrl, Shift, Enter.
        function capsOf(key) {
            const names = {Return: "Enter", Escape: "Esc", Backspace: "⌫", Delete: "Del"}
            const parts = key.length > 1 && key.endsWith("+") ? key.slice(0, -1).split("+").concat(["+"]) : key.split("+")
            return parts.filter(p => p !== "").map(p => names[p] || p)
        }

        contentItem: ColumnLayout {
            spacing: 0
            KeyShortcuts { awake: help.opened }
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true

            RowLayout {
                Layout.fillWidth: true
                spacing: 4 * root.u
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: root.u
                    Text {
                        text: qsTr("Shortcuts")
                        color: root.look.text
                        font.pixelSize: root.look.overlayTitleSize
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: root.look.label
                        font.pixelSize: root.look.textSize
                        text: qsTr("Matches ComfyUI except where marked. Greyed commands are not built yet.")
                    }
                }
                // Narrows the list as you type.
                Rectangle {
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: 60 * root.u
                    Layout.preferredHeight: 8 * root.u
                    radius: Math.min(root.look.popupCorner, 4)
                    color: root.look.well
                    border.width: 1
                    border.color: filterField.activeFocus ? root.look.accent : root.look.hairlineStrong
                    Glyph {
                        id: filterGlyph
                        x: 3 * root.u
                        anchors.verticalCenter: parent.verticalCenter
                        name: "search"
                        look: root.look
                        color: root.look.label
                        font.pixelSize: 12
                    }
                    TextInput {
                        id: filterField
                        objectName: "helpFilter"
                        x: filterGlyph.x + filterGlyph.width + 2 * root.u
                        width: parent.width - x - 2 * root.u
                        anchors.verticalCenter: parent.verticalCenter
                        clip: true
                        selectByMouse: true
                        color: root.look.text
                        selectionColor: root.look.selectedRow
                        selectedTextColor: root.look.text
                        font.pixelSize: root.look.textSize
                        Accessible.role: Accessible.EditableText
                        Accessible.name: qsTr("Filter the shortcuts")
                        onTextEdited: help.filter = text
                        Text {
                            visible: filterField.text === ""
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Filter")
                            color: root.look.label
                            font: filterField.font
                        }
                    }
                }
            }
            Repeater {
                model: App.commands.problems
                delegate: Text {
                    required property string modelData
                    Layout.fillWidth: true
                    Layout.topMargin: root.u
                    wrapMode: Text.Wrap
                    color: root.look.error
                    text: modelData
                }
            }

            Flickable {
                id: commandList
                objectName: "commandList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: 4 * root.u
                Layout.preferredHeight: columnsRow.implicitHeight
                contentHeight: columnsRow.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                Row {
                    id: columnsRow
                    width: commandList.width
                    spacing: 9 * root.u
                    Repeater {
                        model: help.columns
                        delegate: Column {
                            id: column
                            required property var modelData
                            required property int index
                            width: (columnsRow.width - columnsRow.spacing) / 2
                            spacing: 0
                            Repeater {
                                model: column.modelData
                                delegate: Column {
                                    id: group
                                    required property var modelData
                                    required property int index
                                    readonly property var rows: help.rowsOf(modelData)
                                    visible: group.rows.length > 0
                                    width: column.width
                                    Text {
                                        width: parent.width
                                        topPadding: group.index > 0 ? 4 * root.u : 0
                                        bottomPadding: root.u
                                        text: group.modelData.title
                                        color: root.look.label
                                        font.pixelSize: root.look.captionSize
                                        font.weight: Font.DemiBold
                                    }
                                    Repeater {
                                        model: group.rows
                                        delegate: Item {
                                            id: entry
                                            required property var modelData
                                            required property int index
                                            width: group.width
                                            height: 7 * root.u
                                            opacity: modelData.available ? 1 : 0.5
                                            Text {
                                                anchors.left: parent.left
                                                anchors.right: badges.left
                                                anchors.rightMargin: 2 * root.u
                                                anchors.verticalCenter: parent.verticalCenter
                                                text: entry.modelData.description
                                                elide: Text.ElideRight
                                                color: root.look.text
                                                font.pixelSize: root.look.textSize
                                            }
                                            Row {
                                                id: badges
                                                objectName: column.index === 0 && group.index === 0 && entry.index === 0
                                                            ? "helpKeys" : ""
                                                anchors.right: parent.right
                                                anchors.verticalCenter: parent.verticalCenter
                                                spacing: root.u
                                                // Where ComfyUI differs (12.4).
                                                Rectangle {
                                                    visible: entry.modelData.badge !== ""
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    width: badgeText.implicitWidth + 3 * root.u
                                                    height: 4.5 * root.u
                                                    radius: Math.min(root.look.popupCorner, 8)
                                                    color: "transparent"
                                                    border.width: 1
                                                    border.color: root.look.hairlineStrong
                                                    Text {
                                                        id: badgeText
                                                        anchors.centerIn: parent
                                                        text: entry.modelData.badge
                                                        color: root.look.label
                                                        font.pixelSize: root.look.captionSize
                                                    }
                                                }
                                                Repeater {
                                                    model: entry.modelData.gesture ? [] : help.capsOf(entry.modelData.key)
                                                    delegate: Keycap {
                                                        required property string modelData
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        look: root.look
                                                        text: modelData
                                                    }
                                                }
                                                Text {
                                                    visible: entry.modelData.gesture
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    text: entry.modelData.key
                                                    color: root.look.label
                                                    font.pixelSize: root.look.labelSize
                                                }
                                            }
                                            Rectangle {
                                                anchors.bottom: parent.bottom
                                                width: parent.width
                                                height: 1
                                                color: root.look.hairline
                                                opacity: 0.6
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 4 * root.u
                Layout.preferredHeight: 1
                color: root.look.hairline
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4 * root.u
                spacing: root.u
                // One sentence for translators, the path in the monospace face (StyledText has
                // no <tt>, so this is rich text).
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: root.look.label
                    font.pixelSize: root.look.labelSize
                    textFormat: Text.RichText
                    text: qsTr("Change keys in %1").arg("<tt>" + App.commands.keymapFile.replace(/&/g, "&amp;")
                                                         .replace(/</g, "&lt;").replace(/>/g, "&gt;") + "</tt>")
                }
                Keycap { look: root.look; text: "?" }
                Text {
                    text: qsTr("or")
                    color: root.look.label
                    font.pixelSize: root.look.labelSize
                }
                Keycap { look: root.look; text: "Esc" }
                Text {
                    text: qsTr("to close")
                    color: root.look.label
                    font.pixelSize: root.look.labelSize
                }
            }
        }
    }
}
