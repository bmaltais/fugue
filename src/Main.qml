import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "canvas"
import "workflow"

ApplicationWindow {
    id: win
    width: 960
    height: 680
    minimumWidth: 640
    minimumHeight: 460
    visible: true
    title: workflows.windowTitle
    color: theme.background
    font.pixelSize: theme.textSize

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            WorkflowTabs {
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
            }
            FugueCanvas {
                objectName: "canvas"
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: canvasModel
            }
        }

        // The file pickers, the save and trust and recovery questions, the drop area.
        WorkflowDialogs { anchors.fill: parent }
    }

    // The command registry's shortcuts, the `?` overlay and the quit question.
    CommandUi { anchors.fill: parent }
}
