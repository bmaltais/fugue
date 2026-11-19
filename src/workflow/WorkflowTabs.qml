import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The tab bar (spec 12.10): one tab per open workflow, a dot on the ones with unsaved
// content, a close button on each and a button for a new workflow. Needs the `workflows` and
// `theme` context properties. Plain controls; a design pass restyles them.
Rectangle {
    id: root
    objectName: "workflowTabs"
    // Shown from the second workflow on; one workflow needs no tabs, and the window title
    // carries its name and unsaved mark.
    visible: workflows.tabs.length > 1
    implicitHeight: visible ? 36 : 0
    color: theme.panel

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft

    RowLayout {
        anchors.fill: parent
        spacing: 0
        LayoutMirroring.enabled: root.rightToLeft
        LayoutMirroring.childrenInherit: true

        Repeater {
            model: workflows.tabs
            delegate: AbstractButton {
                id: tabButton
                required property var modelData
                required property int index
                objectName: "tab-" + index
                checkable: false
                focusPolicy: Qt.NoFocus
                implicitWidth: 160
                implicitHeight: 36
                onClicked: workflows.current = index
                background: Rectangle {
                    color: tabButton.modelData.current ? theme.background : "transparent"
                    border.color: theme.border
                    border.width: tabButton.modelData.current ? 1 : 0
                }
                contentItem: RowLayout {
                    spacing: 6
                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 8
                        text: (tabButton.modelData.unsaved ? "● " : "") + tabButton.modelData.title
                        elide: Text.ElideMiddle
                        color: theme.foreground
                    }
                    ToolButton {
                        objectName: "closeTab-" + tabButton.index
                        text: "×"
                        implicitWidth: 24
                        implicitHeight: 24
                        padding: 0
                        focusPolicy: Qt.NoFocus
                        Accessible.name: qsTr("Close %1").arg(tabButton.modelData.title)
                        onClicked: workflows.closeTab(tabButton.index)
                    }
                }
            }
        }

        ToolButton {
            objectName: "newTab"
            text: "+"
            focusPolicy: Qt.NoFocus
            Accessible.name: qsTr("New workflow")
            onClicked: workflows.newWorkflow()
        }
        Item { Layout.fillWidth: true }
    }
}
