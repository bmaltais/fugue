import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 960
    height: 680
    minimumWidth: 640
    minimumHeight: 460
    visible: true
    title: "Fugue"
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft

    Shortcut { sequence: "?"; onActivated: help.opened ? help.close() : help.open() }
    Shortcut { sequence: "Q"; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; onActivated: backend.primaryAction() }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        // The app's one job goes here.
        ColumnLayout {
            anchors.centerIn: parent
            spacing: 24

            Label {
                objectName: "count"
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("%Ln presses", "", backend.count)
                font.pixelSize: 32
            }

            Button {
                id: countButton
                objectName: "countButton"
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Count")
                highlighted: true
                // Material.roundedScale isn't inherited, and takes a binding,
                // not a literal number.
                Material.roundedScale: theme.radius
                focusPolicy: Qt.NoFocus
                onClicked: backend.primaryAction()
                // A highlighted button colors its caption by Material's own
                // rule, not Material.foreground, so it can't read on every accent.
                contentItem: Label {
                    text: countButton.text
                    font: countButton.font
                    color: theme.accentForeground
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
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
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }

        ColumnLayout {
            spacing: 8
            // A popup is not an Item and doesn't inherit the window's
            // mirroring, so its content opts in.
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Repeater {
                model: [
                    { keys: "Space", action: qsTr("Count one more") },
                    { keys: "?", action: qsTr("Show or hide the shortcuts") },
                    { keys: "Q", action: qsTr("Quit") }
                ]
                delegate: RowLayout {
                    required property var modelData
                    spacing: 24

                    Label {
                        Layout.preferredWidth: 64
                        text: modelData.keys
                        color: theme.accent
                        font.bold: true
                    }
                    Label { text: modelData.action }
                }
            }
        }
    }
}
