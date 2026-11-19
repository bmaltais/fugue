import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Everything the command registry shows or listens to in the window (spec 12.4, 12.5): one
// shortcut for each key that has an owner, the `?` overlay, the unsaved-work question on
// quit, and the notice when the keymap file could not be used. Needs the `commands` and
// `theme` context properties.
Item {
    id: root
    objectName: "commandUi"

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    property alias overlay: help
    property alias quitDialog: quitAsk

    // A text field that has focus owns bare-letter and Shift-only keys.
    Binding {
        target: commands
        property: "textFieldFocus"
        value: root.Window.window ? commands.isTextInput(root.Window.window.activeFocusItem) : false
    }

    // One shortcut for each key that has an owner. Qt Quick turns a window's shortcuts off
    // while a modal popup is open, so each popup carries its own set (`?` closes the overlay
    // it opened; Ctrl+Q quits from the question), and the window's set sleeps meanwhile so
    // that a key never fires twice.
    component KeyShortcuts: Instantiator {
        property bool active: true
        model: commands.activeKeys
        delegate: Shortcut {
            required property string modelData
            sequence: modelData
            enabled: active && !(commands.textFieldFocus && commands.belongsToTextField(modelData))
            onActivated: commands.triggerKey(modelData)
        }
    }

    KeyShortcuts { active: !help.visible && !quitAsk.visible }

    Connections {
        target: commands
        function onShortcutsToggleRequested() { help.opened ? help.close() : help.open() }
        function onQuitConfirmationRequested() { quitAsk.open() }
    }

    // The keymap file could not be used (or holds mistakes): the defaults still work.
    Rectangle {
        id: notice
        objectName: "keymapNotice"
        property bool dismissed: false
        visible: commands.problems.length > 0 && !dismissed
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: visible ? noticeRow.implicitHeight + 16 : 0
        color: theme.colors.red
        RowLayout {
            id: noticeRow
            anchors { fill: parent; margins: 8 }
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "white"
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
        padding: 24
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }
        ColumnLayout {
            spacing: 16
            KeyShortcuts { active: quitAsk.opened }
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label { text: qsTr("There is unsaved work. Quit anyway?") }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button { text: qsTr("Keep working"); onClicked: quitAsk.close() }
                Button {
                    objectName: "quitAnyway"
                    text: qsTr("Quit")
                    highlighted: true
                    onClicked: commands.quitNow()
                }
            }
        }
    }

    Popup {
        id: help
        objectName: "help"
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        padding: 24
        width: Math.min(root.width - 48, 720)
        height: Math.min(root.height - 48, 560)
        // The registry is read each time the overlay opens, so a later registration or a
        // changed language shows.
        property var entries: []
        onAboutToShow: entries = commands.list()
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            KeyShortcuts { active: help.opened }
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true

            Label {
                text: qsTr("Shortcuts")
                font.bold: true
                font.pixelSize: 18
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                text: qsTr("Matches ComfyUI except where marked. Greyed commands are not built yet.")
            }
            Repeater {
                model: commands.problems
                delegate: Label {
                    required property string modelData
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: theme.colors.red
                    text: modelData
                }
            }

            ListView {
                id: rows
                objectName: "commandList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: help.entries
                ScrollBar.vertical: ScrollBar {}
                delegate: RowLayout {
                    id: row
                    required property var modelData
                    width: ListView.view.width
                    spacing: 16
                    opacity: modelData.available ? 1 : 0.5

                    Label {
                        Layout.preferredWidth: 220
                        Layout.alignment: Qt.AlignTop
                        objectName: "helpKeys"
                        wrapMode: Text.Wrap
                        text: modelData.keys.concat(modelData.gestures).join(", ")
                        color: theme.accent
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: modelData.description
                    }
                    Label {
                        // Where ComfyUI differs (12.4), or the sign that nothing runs yet.
                        visible: modelData.deviates
                        text: modelData.versus
                        font.italic: true
                        opacity: 0.8
                    }
                }
            }
        }
    }
}
