import QtQuick
import QtQuick.Controls
import Fugue

// The border between two areas of a branch (#217 slice 1, spec 12.16; docs/ui-ergonomics.md): a press target
// `thickness` px across the border (24 or more, the Floor), drawn as a 1 px hairline, the accent while it is
// pressed, hovered or has the keyboard focus. AreaNode.qml makes one per border while the window has areas, and
// places it centered on the border.
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
    property real thickness: Math.max(24, 6 * look.unit)
    property real step: 2 * look.unit
    readonly property bool row: axis === "row"
    readonly property bool lit: mouse.pressed || mouse.containsMouse || activeFocus
    signal moveRequested(real delta)
    signal equalizeRequested

    activeFocusOnTab: true
    Accessible.role: Accessible.Splitter
    Accessible.name: row ? qsTr("Border between the areas side by side") : qsTr("Border between the areas one over the other")
    Accessible.description: qsTr("Arrow keys move it; Enter shares the space equally")

    // Where the pointer wants the border: its distance from the border along the axis, from the start edge.
    function ask(along) {
        const border = row ? x + width / 2 : y + height / 2
        const delta = along - border
        if (delta !== 0)
            moveRequested(row && rightToLeft ? -delta : delta)
    }

    // Panes: a 1px divider always. Cards: the gap of window background is the divider, and the line shows only while the border is
    // in use (pointer, press or keys).
    Rectangle {
        visible: handle.look.params.areaLayout === "panes" || handle.lit
        x: handle.row ? Math.round(handle.width / 2) - (handle.lit ? 1 : 0) : 0
        y: handle.row ? 0 : Math.round(handle.height / 2) - (handle.lit ? 1 : 0)
        width: handle.row ? (handle.lit ? 2 : 1) : handle.width
        height: handle.row ? handle.height : (handle.lit ? 2 : 1)
        color: handle.lit ? handle.look.accent : handle.look.hairline
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: handle.row ? Qt.SplitHCursor : Qt.SplitVCursor
        onPressed: handle.forceActiveFocus()
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
    ToolTip.text: qsTr("Drag to resize; double-click to share equally")
}
