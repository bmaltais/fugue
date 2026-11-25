import QtQuick
import QtQuick.Controls
import Fugue

// The border between two areas of a branch (#217 slice 1, spec 12.16; docs/ui-ergonomics.md; canvas-look 20, Border handle):
// a press target `thickness` px across the border (the gap between the cards and 1u into each, at least 4u: an edge, hit along
// its length, as the side panel's grip), with the split cursor. It draws no line along the border (owner, 2026-11-25: "no white
// drag bar"): a small grip pill centred on it says it can be dragged, brighter under the pointer and while dragged, in the accent
// only when Tab gave it the focus. Panes keep their 1px divider. AreaNode.qml makes one per border while the window has areas,
// and places it centered on the border; the App view puts one between its stage and its inputs.
//
// A drag asks for the border under the pointer; the arrow keys along the axis move it by `step` px (Shift: five
// times); a double click or Enter gives the two areas equal shares. It moves nothing itself: it asks with
// `moveRequested(delta)`, the delta along the branch from its start edge (so mirrored here for a row in a
// right-to-left window), and its owner keeps both sides at their minimum.
Item {
    id: handle
    objectName: "areaHandle"
    required property CanvasLook look
    // "row": a border between areas side by side (a vertical line); "column": between areas one over the other.
    property string axis: "row"
    property bool rightToLeft: false
    property real thickness: Math.max(4 * look.unit, (look.params.areaLayout === "panes" ? 0 : look.params.areaGap * look.unit) + 2 * look.unit)
    property real step: 2 * look.unit
    readonly property bool row: axis === "row"
    readonly property bool lit: mouse.pressed || mouse.containsMouse || keyFocus
    // The focus a press gives draws nothing; only Tab's does (KitSegmented).
    property bool pressFocus: false
    onActiveFocusChanged: if (!activeFocus) pressFocus = false
    readonly property bool keyFocus: activeFocus && !pressFocus
    readonly property bool pressed: mouse.pressed
    // The words, for a border that is not between two areas (the App view's inputs panel, AppPage.qml).
    property string name: row ? qsTr("Border between the areas side by side") : qsTr("Border between the areas one over the other")
    property string description: qsTr("Arrow keys move it; Enter shares the space equally")
    property string tip: qsTr("Drag to resize; double-click to share equally")
    signal moveRequested(real delta)
    signal equalizeRequested

    activeFocusOnTab: true
    Accessible.role: Accessible.Splitter
    Accessible.name: name
    Accessible.description: description

    // Where the pointer wants the border: its distance from the border along the axis, from the start edge.
    function ask(along) {
        const border = row ? x + width / 2 : y + height / 2
        const delta = along - border
        if (delta !== 0)
            moveRequested(row && rightToLeft ? -delta : delta)
    }

    // Panes: the 1px divider, always and only that line. Cards: the gap of window background is the divider.
    Rectangle {
        visible: handle.look.params.areaLayout === "panes"
        x: handle.row ? Math.round(handle.width / 2) : 0
        y: handle.row ? 0 : Math.round(handle.height / 2)
        width: handle.row ? 1 : handle.width
        height: handle.row ? handle.height : 1
        color: handle.look.hairline
    }
    // The grip: a 1u x 8u pill in `label` at the scroll bar thumb's alphas (rest, under the pointer or dragged), the accent and 12u
    // long for Tab's focus; left out on a border shorter than 24u.
    Rectangle {
        objectName: "areaHandleGrip"
        readonly property real along: (handle.keyFocus ? 12 : 8) * handle.look.unit
        visible: (handle.row ? handle.height : handle.width) >= 24 * handle.look.unit
        anchors.centerIn: parent
        width: handle.row ? handle.look.unit : along
        height: handle.row ? along : handle.look.unit
        radius: handle.look.unit / 2
        color: handle.keyFocus ? handle.look.accent
             : Qt.alpha(handle.look.label, mouse.pressed || mouse.containsMouse ? 0.7 : handle.look.light ? 0.45 : 0.35)
        Behavior on color { ColorAnimation { duration: handle.look.params.durColor } }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: handle.row ? Qt.SplitHCursor : Qt.SplitVCursor
        onPressed: {
            if (!handle.activeFocus)
                handle.pressFocus = true
            handle.forceActiveFocus(Qt.MouseFocusReason)
        }
        onPositionChanged: mouseEvent => {
            if (!pressed)
                return
            const at = mapToItem(handle.parent, mouseEvent.x, mouseEvent.y)
            handle.ask(handle.row ? at.x : at.y)
        }
        onDoubleClicked: handle.equalizeRequested()
    }
    Keys.onPressed: event => {
        const by = (event.modifiers & Qt.ShiftModifier ? 5 : 1) * step
        // On the screen: left and up are toward smaller x and y.
        let visual = 0
        if (row && event.key === Qt.Key_Left) visual = -by
        else if (row && event.key === Qt.Key_Right) visual = by
        else if (!row && event.key === Qt.Key_Up) visual = -by
        else if (!row && event.key === Qt.Key_Down) visual = by
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            equalizeRequested()
            event.accepted = true
            return
        } else
            return
        moveRequested(row && rightToLeft ? -visual : visual)
        event.accepted = true
    }
    ToolTip.visible: mouse.containsMouse && !mouse.pressed
    ToolTip.delay: App.theme.look.tooltipDelay
    ToolTip.text: tip
}
