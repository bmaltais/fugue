pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../canvas"

// A dockable side panel (#56, ComfyUI's sidebar): a title bar with a button that moves it to the
// other side and one that closes it, the content below it, and a grip on the edge that faces the
// canvas to change its width. It knows nothing of what it holds or of the canvas: put the content
// inside it (it fills the area under the title bar), and the caller sets its place and size.
//
//   look, title, dock ("left" or "right": the window's side, not mirrored), minWidth, maxWidth
//   closeRequested, dockRequested(dock), widthRequested(width): what a person asked for; the caller
//     decides (and keeps) the answer, so the panel never changes its own width or side
//   focusFirst(): the focus goes to the content's `focusTarget`, else the panel
//
// The width is the caller's: the panel is `width` wide as set. The grip also answers Left and
// Right (16 px, Shift 48) when it has the focus (Tab), so a width needs no pointer.
Item {
    id: panel
    objectName: "sidePanel"
    required property CanvasLook look
    property string title
    property string dock: "left"
    property int minWidth: 200
    property int maxWidth: 640
    // The item inside that takes the focus first (a search field, a list); the content sets it.
    property Item focusTarget: null
    default property alias content: body.data
    // Where a caller that makes its content later (createObject) puts it.
    readonly property Item contentArea: body

    signal closeRequested
    signal dockRequested(string dock)
    signal widthRequested(int width)

    readonly property real u: look.unit
    readonly property bool onLeft: dock !== "right"
    readonly property real gripWidth: 8

    function focusFirst() {
        if (focusTarget)
            focusTarget.forceActiveFocus()
        else
            panel.forceActiveFocus()
    }
    function askWidth(wanted) {
        widthRequested(Math.max(minWidth, Math.min(maxWidth, Math.round(wanted))))
    }

    Keys.onEscapePressed: panel.closeRequested()

    Rectangle {
        anchors.fill: parent
        color: panel.look.popup
        // Nothing under the panel hears a press or the wheel that lands on it.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: wheel => wheel.accepted = true
        }
    }
    // The edge that faces the canvas. Placed by x: the window mirrors, the panel's side does not.
    Rectangle {
        x: panel.onLeft ? panel.width - 1 : 0
        width: 1
        height: parent.height
        color: panel.look.hairline
    }

    Item {
        id: bar
        // Under the grip's edge nothing is drawn; the bar spans the panel.
        width: parent.width
        height: 9 * panel.u
        Text {
            anchors { left: parent.left; leftMargin: 3 * panel.u; right: buttons.left; rightMargin: panel.u; verticalCenter: parent.verticalCenter }
            text: panel.title
            elide: Text.ElideRight
            color: panel.look.text
            font.pixelSize: panel.look.labelSize
            font.weight: Font.DemiBold
            Accessible.role: Accessible.Heading
        }
        Row {
            id: buttons
            anchors { right: parent.right; rightMargin: 2 * panel.u; verticalCenter: parent.verticalCenter }
            spacing: panel.u
            ViewButton {
                objectName: "sidePanelDock"
                look: panel.look
                glyph: panel.onLeft ? "chevron-right" : "chevron-left"
                tip: panel.onLeft ? qsTr("Move to the right") : qsTr("Move to the left")
                onTapped: panel.dockRequested(panel.onLeft ? "right" : "left")
            }
            ViewButton {
                objectName: "sidePanelClose"
                look: panel.look
                glyph: "x"
                tip: qsTr("Close")
                onTapped: panel.closeRequested()
            }
        }
    }
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: bar.bottom }
        height: 1
        color: panel.look.hairline
    }
    Item {
        id: body
        // The grip lies on the panel's outer edge and takes the presses there, so the content stops
        // short of it: a scroll bar or a row button under the grip could not be pressed.
        anchors {
            left: parent.left; right: parent.right; top: bar.bottom; bottom: parent.bottom; topMargin: 1
            leftMargin: panel.onLeft ? 0 : panel.gripWidth
            rightMargin: panel.onLeft ? panel.gripWidth : 0
        }
    }

    // The width grip: on the canvas's edge, full height; 8 px wide, which is under the 24 px floor
    // by design (it is an edge, and the keys answer too: docs/ui-ergonomics.md).
    Item {
        id: grip
        objectName: "sidePanelGrip"
        x: panel.onLeft ? panel.width - width : 0
        width: panel.gripWidth
        height: parent.height
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: qsTr("Panel width")
        Rectangle {
            x: panel.onLeft ? parent.width - width : 0
            width: grip.activeFocus || drag.pressed || hover.hovered ? 3 : 0
            height: parent.height
            color: panel.look.accent
            opacity: drag.pressed || grip.activeFocus ? 1 : 0.6
        }
        HoverHandler {
            id: hover
            cursorShape: Qt.SplitHCursor
        }
        MouseArea {
            id: drag
            anchors.fill: parent
            cursorShape: Qt.SplitHCursor
            property real startX: 0
            property real startWidth: 0
            onPressed: mouse => {
                grip.forceActiveFocus(Qt.MouseFocusReason)
                startX = grip.mapToItem(null, mouse.x, 0).x
                startWidth = panel.width
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const dx = grip.mapToItem(null, mouse.x, 0).x - startX
                panel.askWidth(startWidth + (panel.onLeft ? dx : -dx))
            }
        }
        Keys.onLeftPressed: event => panel.askWidth(panel.width + (panel.onLeft ? -1 : 1) * (event.modifiers & Qt.ShiftModifier ? 48 : 16))
        Keys.onRightPressed: event => panel.askWidth(panel.width + (panel.onLeft ? 1 : -1) * (event.modifiers & Qt.ShiftModifier ? 48 : 16))
    }
}
