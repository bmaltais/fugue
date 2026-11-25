import QtQuick
import Fugue
import "../canvas"

// The trash can at the far end of a library row (#156): a 7u = 28px square with a Lucide glyph,
// reachable by Tab and activated by Enter or Space as well as a click. Made only on the rows that
// need one (a Loader), so the list keeps its item budget. It takes the press itself: the row's own
// click and drag (add a node, take it out of the panel) never see one that starts here.
Item {
    id: root
    required property CanvasLook look // App.theme.look
    property string label // for screen readers and the tooltip, "Delete <name> from the library"
    readonly property bool hovered: hover.hovered

    signal clicked

    width: 7 * look.unit
    height: 7 * look.unit
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: label
    Accessible.onPressAction: clicked()

    Rectangle {
        anchors.fill: parent
        radius: root.look.smallCorner
        color: tap.pressed ? root.look.dangerHover : root.hovered ? root.look.dangerFill : "transparent"
        border.width: root.activeFocus && FocusInput.keyboard ? 1 : 0
        border.color: root.look.accent
    }
    Glyph {
        anchors.centerIn: parent
        name: "trash-2"
        look: root.look
        color: root.hovered || root.activeFocus ? root.look.error : root.look.label
    }
    HoverHandler {
        id: hover
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        id: tap
        // The press is this button's alone: no other handler of the row takes it.
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: {
            root.forceActiveFocus()
            root.clicked()
        }
    }
    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()
    Keys.onSpacePressed: clicked()
}
