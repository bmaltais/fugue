pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// The tab bar (spec 12.10): one tab per open workflow, a dot on the ones with unsaved
// content, a close button on each and a button for a new workflow. Reads App.workflows and
// App.theme. docs/canvas-look.md has no section for it: it takes the same
// tokens, a recessed strip with the current tab raised in the node body's fill. Each tab has
// its own color (App.workflows.tabs[i].color, an index into look.tabColors): a bar along its top
// (2px, 4px on the current tab), and a light wash of it behind the tab. A right click makes the tab
// current and opens its menu (#59, menus.h "tab": rename, duplicate, color, save, close, close others),
// built from the command registry, so every item is a command that acts on the current tab.
Rectangle {
    id: root
    objectName: "workflowTabs"
    // Shown from the second workflow on; one workflow needs no tabs, and the window title
    // carries its name and unsaved mark.
    // Off while the Bars and panels menu has them hidden (#129).
    property bool allowed: true
    visible: allowed && App.workflows.tabs.length > 1
    implicitHeight: visible ? 9 * look.unit : 0
    color: look.well

    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    readonly property CanvasLook look: App.theme.look
    readonly property var tabColors: look.tabColors
    function colorOf(index) { return tabColors[((index % tabColors.length) + tabColors.length) % tabColors.length] }

    // Made from the file the first time a tab's menu opens, as the canvas's context menu is.
    property var menu: null
    function openMenu(tab, x, y) {
        if (!root.menu)
            root.menu = Qt.createComponent("../menu/CommandMenu.qml").createObject(root, {objectName: "tabMenuPopup"})
        App.workflows.current = tab.index
        root.menu.close()
        root.menu.target = {tab: tab.index}
        root.menu.items = App.commands.menu("tab", root.menu.target)
        root.menu.byKey = false
        const p = tab.mapToItem(root, x, y)
        root.menu.popupFit(root, p.x, p.y, 0, 0)
    }

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
                readonly property color tabColor: root.colorOf(modelData.color)
                onClicked: App.workflows.current = index
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => root.openMenu(tabButton, point.position.x, point.position.y)
                }
                Connections {
                    target: App.workflows
                    function onTabColorRequested(index) { if (index === tabButton.index) colorPopup.open() }
                    function onTabRenameRequested(index) {
                        if (index !== tabButton.index)
                            return
                        renameField.text = tabButton.modelData.title
                        renamePopup.open()
                    }
                }
                Popup {
                    id: renamePopup
                    objectName: "tabRenamePopup-" + tabButton.index
                    y: tabButton.height
                    padding: root.look.unit * 2
                    focus: true
                    onOpened: { renameField.forceActiveFocus(); renameField.selectAll() }
                    background: Rectangle {
                        color: root.look.popup
                        radius: root.look.popupCorner
                        border.color: root.look.hairlineStrong
                    }
                    contentItem: TextField {
                        id: renameField
                        objectName: "tabRenameField-" + tabButton.index
                        implicitWidth: 40 * root.look.unit
                        selectByMouse: true
                        Accessible.name: qsTr("Workflow name")
                        onAccepted: {
                            if (App.workflows.renameTab(tabButton.index, text))
                                renamePopup.close()
                        }
                    }
                }
                // The top corners take the app's corner radius (look.corner, as nodes and frames do). The
                // tab color is the whole rounded shape, and the fill sits over it a bar's height lower, so
                // the bar follows the curve; both run below the strip's edge, which clips them square.
                background: Item {
                    id: shape
                    clip: true
                    readonly property int barHeight: tabButton.modelData.current ? 4 : 2
                    Rectangle {
                        objectName: "tabColorBar"
                        width: parent.width
                        height: parent.height + root.look.corner
                        radius: root.look.corner
                        color: tabButton.tabColor
                    }
                    Rectangle {
                        y: shape.barHeight
                        width: parent.width
                        height: parent.height + root.look.corner
                        radius: root.look.corner
                        color: Qt.tint(tabButton.modelData.current ? root.look.body : root.look.well,
                                       Qt.alpha(tabButton.tabColor, tabButton.modelData.current ? 0.12 : 0.07))
                    }
                    Rectangle {
                        y: shape.barHeight
                        width: parent.width
                        height: parent.height + root.look.corner
                        radius: root.look.corner
                        color: Qt.alpha(root.look.text, 0.04)
                        visible: tabButton.hovered && !tabButton.modelData.current
                    }
                    Rectangle {
                        anchors.right: parent.right
                        width: 1
                        height: parent.height
                        color: root.look.hairline
                    }
                }
                Popup {
                    id: colorPopup
                    objectName: "tabColorPopup-" + tabButton.index
                    y: tabButton.height
                    padding: root.look.unit * 2
                    background: Rectangle {
                        color: root.look.popup
                        radius: root.look.popupCorner
                        border.color: root.look.hairlineStrong
                    }
                    contentItem: Row {
                        spacing: root.look.unit
                        Repeater {
                            model: root.tabColors.length
                            delegate: AbstractButton {
                                id: swatch
                                required property int index
                                objectName: "tabColor-" + tabButton.index + "-" + index
                                implicitWidth: 7 * root.look.unit
                                implicitHeight: 7 * root.look.unit
                                focusPolicy: Qt.StrongFocus
                                Accessible.name: qsTr("Tab color %1").arg(index + 1)
                                Accessible.checked: tabButton.modelData.color === index
                                onClicked: {
                                    App.workflows.setTabColor(tabButton.index, index)
                                    colorPopup.close()
                                }
                                background: Rectangle {
                                    radius: width / 2
                                    color: root.colorOf(swatch.index)
                                    border.width: tabButton.modelData.color === swatch.index || swatch.activeFocus ? 2 : 0
                                    border.color: root.look.text
                                }
                            }
                        }
                    }
                }
                contentItem: RowLayout {
                    spacing: 1.5 * root.look.unit
                    Rectangle {
                        // Unsaved: a small dot before the title.
                        visible: tabButton.modelData.unsaved
                        Layout.leftMargin: 3 * root.look.unit
                        implicitWidth: 6
                        implicitHeight: 6
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
