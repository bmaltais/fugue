pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue

// The tab strip on the window's left edge (#159; ComfyUI's sidebar rail, VS Code's Activity Bar):
// one icon per registered side panel, so the way in is something to see and click, not a key to
// know. A click opens the panel, a second click closes it; the open one is marked by the accent
// glyph, a wash behind it and a bar on the edge it opens from. Each tab names itself and its key in
// a tooltip, takes Tab focus (a ring in the accent), and Enter or Space presses it.
//
// A tab is a `panel` or an `action` (#192): an action launches a command (Templates opens the
// templates browser, Settings the settings) instead of toggling a panel, so it is never marked
// open, and it is not a checkable button for the screen reader. A row's `edge` says where it sits:
// "top" under the window bars, "bottom" at the lower end past a hairline; the registry decides, this
// file only draws the two groups.
//
//   look; panels: a list of {tab, kind ("panel"|"action"), edge ("top"|"bottom"), name, glyph (a Lucide name), command, keys, open}
//     (App.sidePanels.panels)
//   tabRequested(tab): a panel tab was pressed; the caller opens or closes it (the strip changes nothing)
//   actionRequested(command): an action tab was pressed; the caller runs the command
//
// It is as wide as its tabs (`stripWidth`, 11u = 44px: a 44px hit area under a 24px glyph), as tall
// as it is given, and draws nothing of a panel: it knows only the list it shows, so a new panel is
// a new row there.
Rectangle {
    id: strip
    objectName: "sideTabStrip"
    required property CanvasLook look
    property var panels: []
    signal tabRequested(string tab)
    signal actionRequested(string command)

    readonly property real u: look.unit
    readonly property real tabSize: 11 * u
    readonly property real stripWidth: tabSize
    width: stripWidth
    color: look.popup
    Accessible.role: Accessible.PageTabList
    Accessible.name: qsTr("Side panels")

    // The edge toward the canvas.
    Rectangle {
        x: parent.width - 1
        width: 1
        height: parent.height
        color: strip.look.hairline
    }
    // One tab: the same look and keys for a panel and an action; only what a press asks for differs.
    component Tab: Rectangle {
        id: tab
        required property int index
        // Which end's group this tab is in; `index` counts within it.
        property string edge: "top"
        readonly property var info: strip.group(edge)[index]
        readonly property bool isAction: info.kind === "action"
        readonly property bool open: !isAction && info.open === true
        readonly property string keyText: info.keys && info.keys.length > 0 ? info.keys[0] : ""
        // What the tooltip says: the name, and the key that does the same.
        readonly property string tip: keyText !== "" ? qsTr("%1 (%2)").arg(info.name).arg(keyText) : info.name
        function press() {
            if (isAction)
                strip.actionRequested(info.command)
            else
                strip.tabRequested(info.tab)
        }
        objectName: "sideTab-" + info.tab
        width: strip.stripWidth - 1
        height: strip.tabSize
        color: open ? strip.look.selectedRow : hover.hovered ? strip.look.menuRow : "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: info.name
        Accessible.checkable: !isAction
        Accessible.checked: open
        // The mark of the open one: a bar on the edge the panel opens from.
        Rectangle {
            visible: tab.open
            width: 3
            height: parent.height
            color: strip.look.accent
        }
        Text {
            anchors.centerIn: parent
            text: strip.look.glyph(tab.info.glyph)
            font.family: strip.look.iconFont
            font.pixelSize: 24
            color: tab.open ? strip.look.accent : hover.hovered || tab.activeFocus ? strip.look.text : strip.look.label
        }
        Rectangle {
            anchors { fill: parent; margins: 2 }
            visible: tab.activeFocus && FocusInput.keyboard
            color: "transparent"
            border { width: 2; color: strip.look.accent }
            radius: strip.look.smallCorner
        }
        TapHandler {
            gesturePolicy: TapHandler.WithinBounds
            onTapped: tab.press()
        }
        HoverHandler {
            id: hover
            cursorShape: Qt.PointingHandCursor
        }
        ToolTip.visible: hover.hovered
        ToolTip.text: tip
        ToolTip.delay: App.theme.look.tooltipDelay
        Keys.onReturnPressed: tab.press()
        Keys.onEnterPressed: tab.press()
        Keys.onSpacePressed: tab.press()
    }
    // The rows of one end, in registry order. The tabs are made from the counts, not the list: the
    // list is replaced when a panel opens, and the tabs keep their focus.
    function group(edge: string): var {
        return panels.filter(p => p.edge === edge)
    }
    readonly property int topCount: group("top").length
    readonly property int bottomCount: group("bottom").length
    // The top group, from the top.
    Column {
        anchors { top: parent.top; topMargin: strip.u }
        Repeater {
            model: strip.topCount
            delegate: Tab { edge: "top" }
        }
    }
    // The bottom group, at the lower end past a hairline (when it has any tab).
    Column {
        anchors { bottom: parent.bottom; bottomMargin: strip.u }
        Rectangle {
            objectName: "sideTabActionsRule"
            visible: strip.bottomCount > 0
            x: strip.u
            width: strip.stripWidth - 1 - 2 * strip.u
            height: visible ? 1 : 0
            color: strip.look.hairline
        }
        Item { width: 1; height: strip.u / 2 }
        Repeater {
            model: strip.bottomCount
            delegate: Tab { edge: "bottom" }
        }
    }
}
