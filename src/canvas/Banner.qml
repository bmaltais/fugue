import QtQuick
import Fugue

// A notice with actions (#228, the cohesion audit's C8): the look of the workflow and helper notices (`popup` fill, a
// `hairline` edge, the small corner), 2u inside, its content (a Column: the line, the buttons, a hint) given as children.
// `decision` adds a 3px accent bar at its start for a notice that waits for the person to decide (a new candidate):
// the accent stays the primary's alone as a fill (D7), so the notice is told apart by the bar, not by a tint. It is as
// tall as its content and mirrors right to left with the window (the bar at the end).
Rectangle {
    id: banner
    required property CanvasLook look
    property bool decision: false
    default property alias content: holder.data
    readonly property real inset: 2 * look.unit
    implicitHeight: holder.childrenRect.height + 2 * inset
    radius: look.smallCorner
    color: look.popup
    border.width: 1
    border.color: look.hairline
    Rectangle {
        visible: banner.decision
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: 1
        width: 3
        radius: 1
        color: banner.look.accent
    }
    Item {
        id: holder
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: banner.inset + (banner.decision ? 3 : 0)
        anchors.rightMargin: banner.inset
        anchors.topMargin: banner.inset
        height: childrenRect.height
    }
}
