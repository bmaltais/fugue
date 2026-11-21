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
//   look; panels: a list of {tab, name, glyph (a Lucide name), keys, open} (App.sidePanels.panels)
//   tabRequested(tab): a tab was pressed; the caller opens or closes it (the strip changes nothing)
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
    Column {
        anchors { top: parent.top; topMargin: strip.u }
        Repeater {
            // The count, not the list: the list is replaced when a panel opens, and the tabs keep their focus.
            model: strip.panels.length
            delegate: Rectangle {
                id: tab
                required property int index
                readonly property var info: strip.panels[index]
                readonly property bool open: info.open === true
                readonly property string keyText: info.keys && info.keys.length > 0 ? info.keys[0] : ""
                // What the tooltip says: the name, and the key that does the same.
                readonly property string tip: keyText !== "" ? qsTr("%1 (%2)").arg(info.name).arg(keyText) : info.name
                objectName: "sideTab-" + info.tab
                width: strip.stripWidth - 1
                height: strip.tabSize
                color: open ? strip.look.selectedRow : hover.hovered ? strip.look.menuRow : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: info.name
                Accessible.checkable: true
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
                    visible: tab.activeFocus
                    color: "transparent"
                    border { width: 2; color: strip.look.accent }
                    radius: strip.look.smallCorner
                }
                TapHandler {
                    onTapped: strip.tabRequested(tab.info.tab)
                }
                HoverHandler {
                    id: hover
                    cursorShape: Qt.PointingHandCursor
                }
                ToolTip.visible: hover.hovered
                ToolTip.text: tip
                ToolTip.delay: 600
                Keys.onReturnPressed: strip.tabRequested(tab.info.tab)
                Keys.onSpacePressed: strip.tabRequested(tab.info.tab)
            }
        }
    }
}
