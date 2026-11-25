pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../kit"

// A notice with actions (#228, the cohesion audit's C8; docs/canvas-look.md 20.21): a calm KitPanel card (the popup fill with a faint tint of
// `tone`, the hairline and the card corner), 2u inside, its content (a Column: the line, the buttons, a hint) given as children. `tone` says
// what the notice is about: slate for the quiet ones, moss for "Get ready" and downloads. It is as tall as its content and mirrors right to
// left with the window.
Item {
    id: banner
    required property CanvasLook look
    property string tone: "slate"
    default property alias content: holder.data
    readonly property real inset: 2 * look.unit
    implicitHeight: holder.childrenRect.height + 2 * inset
    height: implicitHeight
    Loader { // made only while shown: the window keeps few items (#5), and a notice is mostly hidden
        anchors.fill: parent
        active: banner.visible
        sourceComponent: KitPanel {
            look: banner.look
            tone: banner.tone
            calm: true
            radius: panes ? 0 : Math.min(look.params.cornerGroup * look.unit, 0.5 * height)
        }
    }
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
