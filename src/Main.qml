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

    // The command registry's shortcuts, the `?` overlay and the quit question.
    CommandUi { anchors.fill: parent }
}
