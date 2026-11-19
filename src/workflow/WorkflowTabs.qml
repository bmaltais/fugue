import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// The tab bar (spec 12.10): one tab per open workflow, a dot on the ones with unsaved
// content, a close button on each and a button for a new workflow. Needs the `workflows` and
// `theme` context properties. docs/canvas-look.md has no section for it: it takes the same
// tokens, a recessed strip with the current tab raised in the node body's fill, marked by
// the accent along its top.
Rectangle {
    id: root
    objectName: "workflowTabs"
    // Shown from the second workflow on; one workflow needs no tabs, and the window title
    // carries its name and unsaved mark.
    visible: App.workflows.tabs.length > 1
    implicitHeight: visible ? 9 * look.unit : 0
    color: look.well

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    readonly property var look: App.theme.look

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: root.look.hairline
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0
        LayoutMirroring.enabled: root.rightToLeft
        LayoutMirroring.childrenInherit: true

        Repeater {
            model: App.workflows.tabs
            delegate: AbstractButton {
                id: tabButton
                required property var modelData
                required property int index
                objectName: "tab-" + index
                checkable: false
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                implicitWidth: 44 * root.look.unit
                Layout.fillHeight: true
                onClicked: App.workflows.current = index
                background: Rectangle {
                    color: tabButton.modelData.current ? root.look.body
                         : tabButton.hovered ? Qt.tint(root.look.well, Qt.alpha(root.look.text, 0.04)) : "transparent"
                    Rectangle {
                        visible: tabButton.modelData.current
                        width: parent.width
                        height: 2
                        color: root.look.accent
                    }
                    Rectangle {
                        anchors.right: parent.right
                        width: 1
                        height: parent.height
                        color: root.look.hairline
                    }
                }
                contentItem: RowLayout {
                    spacing: 1.5 * root.look.unit
                    Rectangle {
                        // Unsaved: a small dot before the title.
                        visible: tabButton.modelData.unsaved
                        Layout.leftMargin: 3 * root.look.unit
                        width: 6
                        height: 6
                        radius: 3
                        color: root.look.accent
                    }
                    Label {
                        objectName: "tabTitle"
                        Layout.fillWidth: true
                        Layout.leftMargin: tabButton.modelData.unsaved ? 0 : 3 * root.look.unit
                        text: tabButton.modelData.title
                        Accessible.name: tabButton.modelData.unsaved ? qsTr("%1, unsaved").arg(text) : text
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        color: tabButton.modelData.current ? root.look.text : root.look.label
                        font.pixelSize: root.look.labelSize
                        font.weight: tabButton.modelData.current ? Font.DemiBold : Font.Normal
                    }
                    ToolButton {
                        objectName: "closeTab-" + tabButton.index
                        implicitWidth: 6 * root.look.unit
                        implicitHeight: 6 * root.look.unit
                        Layout.rightMargin: root.look.unit
                        padding: 0
                        focusPolicy: Qt.NoFocus
                        flat: true
                        Accessible.name: qsTr("Close %1").arg(tabButton.modelData.title)
                        onClicked: App.workflows.closeTab(tabButton.index)
                        contentItem: Glyph {
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            name: "x"
                            look: root.look
                            color: root.look.label
                            font.pixelSize: 12
                        }
                        background: null
                    }
                }
            }
        }

        ToolButton {
            objectName: "newTab"
            Layout.fillHeight: true
            implicitWidth: 9 * root.look.unit
            focusPolicy: Qt.NoFocus
            flat: true
            Accessible.name: qsTr("New workflow")
            onClicked: App.workflows.newWorkflow()
            contentItem: Glyph {
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                name: "plus"
                look: root.look
                color: root.look.label
            }
            background: null
        }
        Item { Layout.fillWidth: true }
    }
}
