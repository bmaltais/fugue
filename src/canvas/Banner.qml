import QtQuick
import Fugue

// A notice with actions (#228, the cohesion audit's C8): the look of the workflow and helper notices (`popup` fill, a
// `hairline` edge, the small corner), 2u inside, its content (a Column: the line, the buttons, a hint) given as children.
// It is as tall as its content and mirrors right to left with the window.
Rectangle {
    id: banner
    required property CanvasLook look
    default property alias content: holder.data
    readonly property real inset: 2 * look.unit
    implicitHeight: holder.childrenRect.height + 2 * inset
    radius: look.smallCorner
    color: look.popup
    border.width: 1
    border.color: look.hairline
    Item {
        id: holder
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: banner.inset
        anchors.rightMargin: banner.inset
        anchors.topMargin: banner.inset
        height: childrenRect.height
    }
}
