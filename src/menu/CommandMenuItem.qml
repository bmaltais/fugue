import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"

// One row of a CommandMenu: a command as the registry shows it (`entry`: label, keys,
// available, enabled, checkable, checked, and swatch on a color), or the row that opens a
// submenu. A left gutter holds the check of a toggle or a color's swatch; the first key is
// at the right in `label`. A row that is off, not built yet or not for what is selected, is dimmed.
MenuItem {
    id: row

    // Hover moves the menu's current row, as the keys do (Menu: currentIndex).
    hoverEnabled: true
    property var entry
    property var target: ({})

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    readonly property bool available: entry === undefined || entry.available
    // A planned feature (#232): greyed, no command, its tooltip says which issue builds it.
    readonly property bool planned: entry !== undefined && entry.placeholder !== undefined
    readonly property string plannedNote: planned ? qsTr("Not built yet (#%1)").arg(entry.issue) : ""

    objectName: planned ? "menuPlanned-" + entry.issue + "-" + entry.label : entry !== undefined ? "menuItem-" + entry.command : subMenu ? "subMenu-" + subMenu.title.replace("&", "") : ""
    text: entry !== undefined ? entry.label : subMenu ? subMenu.title : ""
    enabled: entry === undefined || entry.enabled
    checkable: entry !== undefined && entry.checkable
    checked: entry !== undefined && entry.checked
    // Off (not built yet, or not for what is selected) is dimmed, so it reads as off (#72).
    opacity: available && enabled ? 1 : 0.4
    implicitWidth: leftPadding + rightPadding + gutter.width + label.implicitWidth
                   + (keys.text !== "" ? keys.implicitWidth + 6 * u : 0) + (subMenu ? 4 * u : 0)
    implicitHeight: 7 * u
    leftPadding: 2 * u
    // The capped menu scrolls (`interactive`) and shows its bar then; the bar's own visibility would loop through the menu's size.
    rightPadding: 2.5 * u + (ListView.view && ListView.view.interactive ? (ListView.view.ScrollBar.vertical as FugueScrollBar).breadth : 0)
    Accessible.name: text.replace("&", "")
    Accessible.description: planned ? plannedNote : entry !== undefined && !entry.available ? qsTr("Not available yet") : ""
    ToolTip.visible: planned && hovered
    ToolTip.text: plannedNote
    ToolTip.delay: 400

    onTriggered: {
        if (entry === undefined)
            return
        // From a canvas context menu, a paste lands where the menu was opened.
        if (target.x !== undefined)
            App.canvasModel.pointer = Qt.point(target.x, target.y)
        App.commands.triggerOn(entry.command, target)
    }

    indicator: null
    arrow: null
    background: Rectangle {
        radius: Math.max(0, row.look.popupCorner - 1.5 * row.u)
        color: (row.highlighted || (!!row.subMenu && row.subMenu.visible)) && row.enabled ? row.look.menuRow : "transparent"
    }
    contentItem: Item {
        implicitHeight: label.implicitHeight
        LayoutMirroring.enabled: row.rightToLeft
        LayoutMirroring.childrenInherit: true
        Item {
            id: gutter
            width: 5 * row.u
            height: parent.height
            Rectangle {
                visible: row.entry !== undefined && row.entry.swatch !== undefined
                anchors.verticalCenter: parent.verticalCenter
                width: 3 * row.u
                height: width
                radius: Math.min(row.look.smallCorner, 2)
                // The theme's own color for the role, so the swatch names the hue plainly; on
                // the node it becomes the gentler header tint (docs/canvas-look.md 6.1).
                readonly property string role: visible ? row.entry.swatch : "none"
                color: role === "none" ? "transparent" : role === "accent" ? App.theme.accent
                     : role === "muted" ? row.look.label : (App.theme.colors[role] || row.look.label)
                border.width: role === "none" ? 1 : 0
                border.color: row.look.hairlineStrong
            }
            Text {
                visible: row.checkable && row.checked
                anchors.verticalCenter: parent.verticalCenter
                text: row.look.glyph("check")
                font.family: row.look.iconFont
                font.pixelSize: 12
                color: row.look.text
            }
        }
        Text {
            id: label
            anchors.left: gutter.right
            anchors.right: keys.left
            anchors.rightMargin: 4 * row.u
            anchors.verticalCenter: parent.verticalCenter
            text: row.text.replace("&", "")
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font.pixelSize: row.look.textSize
            color: !row.enabled ? row.look.label : row.entry !== undefined && row.entry.danger === true ? row.look.error : row.look.text
        }
        Text {
            id: keys
            anchors.right: chevron.visible ? chevron.left : parent.right
            anchors.verticalCenter: parent.verticalCenter
            // The overlay's key names: Enter, Esc.
            text: row.entry !== undefined && row.entry.keys.length > 0
                  ? row.entry.keys[0].replace("Return", "Enter").replace("Escape", "Esc") : ""
            font.pixelSize: row.look.labelSize
            font.features: {"tnum": 1}
            color: row.look.label
        }
        Text {
            id: chevron
            visible: !!row.subMenu
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: row.look.glyph(row.rightToLeft ? "chevron-left" : "chevron-right")
            font.family: row.look.iconFont
            font.pixelSize: 12
            color: row.look.label
        }
    }
}
