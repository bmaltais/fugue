import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../kit"

// One row of the Get ready sheet (#379, #464), a row of a KitGroup (hairline above unless it is the first, the quiet hover of a list row): a kind glyph, the name, what it takes on the right (size, or the word for its
// state while it runs), a quieter line under the name (licence, source, "type from <pack>"), a thin bar while it
// downloads, and a chevron that opens the rest inline. The whole row is the button: it takes the menu-row fill under the
// pointer and the 2px accent ring from the keyboard, as every list row in the window does. Long text is cut at its end
// and the full line is the tooltip, so nothing needs a horizontal scroll bar.
Rectangle {
    id: row
    required property CanvasLook look // App.theme.look
    property string glyph: "box"
    property string title
    property string subtitle
    // What the row takes (a size), at the right.
    property string trailing
    // The word for the run's state ("Done", "Working…"), left of the size; empty before the run.
    property string stateText
    property bool failed: false
    // The rest, shown below when open; an empty text means there is nothing more and no chevron.
    property string more
    property bool open: false
    // A download's progress, 0 to 1; negative while unknown; `running` shows the bar.
    property bool running: false
    property real fraction: -1
    // Why a row failed, in the error color.
    property string error
    signal toggled

    readonly property bool expandable: more !== ""
    readonly property real u: look.unit
    readonly property bool hovered: hover.hovered && expandable

    width: parent ? parent.width : implicitWidth
    implicitHeight: Math.max(look.params.rowMinHeight * u, column.implicitHeight + 2 * look.params.rowPadY * u)
    height: implicitHeight
    radius: look.smallCorner
    color: hovered ? look.menuRow : "transparent"
    border.width: activeFocus && FocusInput.keyboard ? 2 : 0
    border.color: look.accent
    activeFocusOnTab: expandable
    Accessible.role: expandable ? Accessible.Button : Accessible.ListItem
    Accessible.name: title + (trailing !== "" ? ", " + trailing : "")
    Accessible.description: subtitle
    Accessible.checkable: expandable
    Accessible.checked: open
    Keys.onReturnPressed: if (expandable) row.toggled()
    Keys.onEnterPressed: if (expandable) row.toggled()
    Keys.onSpacePressed: if (expandable) row.toggled()

    KitSeparator {
        look: row.look
        visible: row.y > 0
        anchors.top: parent.top
        z: 1
    }
    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: row.look.params.rowPadY * row.u
        anchors.leftMargin: row.look.params.rowPadLeft * row.u
        anchors.rightMargin: row.look.params.rowPadRight * row.u
        spacing: 0.5 * row.u

        RowLayout {
            Layout.fillWidth: true
            spacing: 2 * row.u
            Glyph {
                look: row.look
                name: row.glyph
                color: row.look.label
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 4 * row.u
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                id: titleText
                Layout.fillWidth: true
                text: row.title
                elide: Text.ElideRight
                color: row.look.text
                font.pixelSize: row.look.textSize
            }
            Text {
                visible: row.stateText !== ""
                Layout.minimumWidth: 8 * row.u
                horizontalAlignment: Text.AlignRight
                text: row.stateText
                color: row.failed ? row.look.error : row.look.label
                font.pixelSize: row.look.labelSize
            }
            Text {
                visible: row.trailing !== ""
                text: row.trailing
                color: row.look.label
                font.pixelSize: row.look.textSize
            }
            Glyph {
                // Reserved even when the row has nothing to open, so every size sits on one edge.
                look: row.look
                name: row.open ? "chevron-down" : "chevron-right"
                visible: row.expandable
                color: row.hovered || row.activeFocus ? row.look.text : row.look.label
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 3 * row.u
            }
            Item {
                visible: !row.expandable
                Layout.preferredWidth: 3 * row.u
            }
        }
        Text {
            id: subtitleText
            visible: row.subtitle !== ""
            Layout.fillWidth: true
            Layout.leftMargin: 6 * row.u
            Layout.rightMargin: 5 * row.u
            text: row.subtitle
            elide: Text.ElideRight
            color: row.look.label
            font.pixelSize: row.look.labelSize
        }
        ProgressBar {
            visible: row.running
            Layout.fillWidth: true
            Layout.leftMargin: 6 * row.u
            Layout.rightMargin: 5 * row.u
            Layout.preferredHeight: 0.75 * row.u
            from: 0
            to: 1
            indeterminate: row.fraction < 0
            value: Math.max(0, row.fraction)
        }
        Text {
            visible: row.open && row.more !== ""
            Layout.fillWidth: true
            Layout.topMargin: row.u
            Layout.leftMargin: 6 * row.u
            Layout.rightMargin: 5 * row.u
            text: row.more
            wrapMode: Text.Wrap
            color: row.look.label
            font.pixelSize: row.look.labelSize
        }
        RowLayout {
            visible: row.error !== ""
            Layout.fillWidth: true
            Layout.leftMargin: 6 * row.u
            Layout.rightMargin: 5 * row.u
            spacing: row.u
            Text {
                Layout.fillWidth: true
                text: row.error
                wrapMode: Text.Wrap
                color: row.look.error
                font.pixelSize: row.look.labelSize
            }
            KitCopyButton {
                objectName: "getReadyRowCopy"
                look: row.look
                Layout.alignment: Qt.AlignTop
                copyText: row.error
            }
        }
    }

    HoverHandler {
        id: hover
        cursorShape: row.expandable ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    TapHandler {
        enabled: row.expandable
        gesturePolicy: TapHandler.WithinBounds
        onTapped: {
            row.forceActiveFocus()
            row.toggled()
        }
    }
    // The cut text in full, after the window's tooltip delay.
    ToolTip.visible: hover.hovered && (titleText.truncated || subtitleText.truncated)
    ToolTip.delay: row.look.tooltipDelay
    ToolTip.text: row.title + (row.subtitle !== "" ? "\n" + row.subtitle : "")
}
