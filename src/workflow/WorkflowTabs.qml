pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// The tab strip (spec 12.10): one tab per open workflow, a dot on the ones with unsaved
// content, a close button on each and a button for a new workflow. It sits in the window bar's row, after the menus and
// before the project chip (#262, as Blender puts its workspace tabs in the top bar): MenuBar.qml gives it the room.
// The tabs take their width while they fit, then share the room down to a least width, then the strip scrolls (the wheel,
// and the current tab is kept in view): the rules are menus.h's (MenuLayout). Reads App.workflows and
// App.theme. docs/canvas-look.md has no section for it: it takes the same
// tokens, the bar's recessed strip with the current tab raised in the node body's fill. Each tab has
// its own color (App.workflows.tabs[i].color, an index into look.tabColors): a bar along its top
// (2px, 4px on the current tab), and a light wash of it behind the tab. A right click makes the tab
// current and opens its menu (#59, menus.h "tab": rename, duplicate, color, save, close, close others, close all),
// built from the command registry, so every item is a command that acts on the current tab.
Item {
    id: root
    objectName: "workflowTabs"
    // Shown from the second workflow on; one workflow needs no tabs, and the window title
    // carries its name and unsaved mark.
    // Off while the Bars and panels menu has them hidden (#129).
    property bool allowed: true
    visible: allowed && App.workflows.tabs.length > 1
    // The strip's own sizes: a tab's width and least, the New button's.
    readonly property real preferredTab: 36 * look.unit
    readonly property real leastTab: 16 * look.unit
    readonly property real newWidth: 7 * look.unit
    readonly property real tabWidth: MenuLayout.tabWidth(App.workflows.tabs.length, width - newWidth, preferredTab, leastTab)
    readonly property real stripWidth: Math.min(App.workflows.tabs.length * tabWidth, Math.max(0, width - newWidth))
    // How far the tabs are scrolled, kept so the current one is whole.
    property real scroll: 0
    function reveal() {
        scroll = MenuLayout.tabScroll(App.workflows.current, App.workflows.tabs.length, tabWidth, stripWidth, scroll)
    }
    onTabWidthChanged: reveal()
    onStripWidthChanged: reveal()
    Connections {
        target: App.workflows
        function onCurrentChanged() { root.reveal() }
    }

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

    LayoutMirroring.enabled: root.rightToLeft
    LayoutMirroring.childrenInherit: true

    // The tabs, clipped to the room; a wheel over them scrolls.
    Item {
        id: strip
        anchors.left: parent.left
        width: root.stripWidth
        height: parent.height
        clip: true
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: event => {
                const delta = event.angleDelta.x !== 0 ? event.angleDelta.x : event.angleDelta.y
                const most = Math.max(0, App.workflows.tabs.length * root.tabWidth - root.stripWidth)
                root.scroll = Math.max(0, Math.min(most, root.scroll - delta / 2))
            }
        }

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
                width: root.tabWidth
                height: strip.height
                x: (root.rightToLeft ? strip.width - (index + 1) * width + root.scroll : index * width - root.scroll)
                readonly property color tabColor: root.colorOf(modelData.color)
                onClicked: App.workflows.current = index
                // A double click renames, as a sheet tab in Excel or Google Sheets does; the two clicks have made the tab current.
                onDoubleClicked: { tabButton.rename() }
                function rename() {
                    renameField.text = tabButton.modelData.title
                    renamePopup.open()
                }
                TapHandler {
                    gesturePolicy: TapHandler.WithinBounds
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => root.openMenu(tabButton, point.position.x, point.position.y)
                }
                Connections {
                    target: App.workflows
                    function onTabColorRequested(index) { if (index === tabButton.index) colorPopup.open() }
                    function onTabRenameRequested(index) {
                        if (index === tabButton.index)
                            tabButton.rename()
                    }
                }
                Popup {
                    id: renamePopup
                    objectName: "tabRenamePopup-" + tabButton.index
                    y: tabButton.height
                    padding: root.look.unit * 2
                    focus: true
                    onOpened: { renameField.forceActiveFocus(); renameField.selectAll() }
                    // The field is the popup (owner, 2026-11-27: a field in a padded card read as a box in a box): one card, its
                    // edge and shadow, the text inside it as an inline rename.
                    background: PopupSurface { look: root.look }
                    contentItem: ThemedTextField {
                        id: renameField
                        objectName: "tabRenameField-" + tabButton.index
                        implicitWidth: 40 * root.look.unit
                        implicitHeight: 6 * root.look.unit
                        leftPadding: root.look.unit // with the popup's padding the text sits 3u in, as a menu row's
                        rightPadding: root.look.unit
                        framed: false
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
                    background: PopupSurface { look: root.look }
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
                    spacing: root.look.unit
                    Rectangle {
                        // Unsaved: a small dot before the title.
                        visible: tabButton.modelData.unsaved
                        Layout.leftMargin: 2 * root.look.unit
                        implicitWidth: 6
                        implicitHeight: 6
                        radius: 3
                        color: root.look.accent
                    }
                    Label {
                        objectName: "tabTitle"
                        Layout.fillWidth: true
                        Layout.leftMargin: tabButton.modelData.unsaved ? 0 : 2 * root.look.unit
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
                        implicitWidth: 5 * root.look.unit
                        implicitHeight: 5 * root.look.unit
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
    }

    // After the last tab, which the strip's end is.
    ToolButton {
        id: newTabButton
        objectName: "newTab"
        anchors.left: strip.right
        height: parent.height
        implicitWidth: root.newWidth
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
}
