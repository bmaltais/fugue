pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue

// The auto-hide container for a bar along the top edge (#160, spec 12.15): the one component any
// bar uses to slide out of the way instead of disappearing. Put the bar inside it (it takes the
// bar's implicit height; the bar fills the container's width).
//
// `pinned` is the bar shown in the layout, the way it always was. Not pinned, the container takes
// only a slim hot edge from the layout (`edgeHeight`) and draws a handle (a menu glyph, 28 px or
// more, hanging below the edge at the leading corner). The bar itself waits just above the window
// and slides down over the content: the content never moves and is not resized while it slides (the
// bar overlays it; the slide moves the bar's `y`, never the layout).
//
//   - Pointer on the edge or the handle for `enterDelay` (or a press on the handle at once): slides in.
//   - Pointer off the bar and edge for `leaveDelay`: slides out; with a menu of the bar open
//     (`holdOpen`) it stays, and slides out when that closes. Touch has no hover, so a bar a tap
//     brought in stays `touchDelay`.
//   - `reveal()` and `conceal()` are for the keys: F10 and Alt mnemonics reveal, Escape conceals (the
//     window forwards it: `escapeWanted`).
//   - `edgeShown` false (focus mode) draws neither the edge nor the handle; reveal() still works.
Item {
    id: root
    objectName: "autoHide"

    // What goes in it: the bar.
    default property alias content: holder.data

    property bool pinned: true
    property bool edgeShown: true
    // A menu of the bar is open: the bar stays in.
    property bool holdOpen: false
    property string handleName: qsTr("Show the bar")
    // The handle sits this far from the leading side (the window's left: a strip or panel there is cleared).
    property real handleInset: 0
    property int edgeHeight: 4
    property int slideDuration: 170
    property int enterDelay: 120
    property int leaveDelay: 400
    property int touchDelay: 4000

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft

    // 0 (out, above the window) to 1 (in), animated.
    property real slide: inWindow ? 1 : 0
    // The bar has been asked in (the slide may still run) and nothing pins it.
    property bool revealed: false
    readonly property bool inWindow: pinned || revealed
    // Escape is the bar's while it is in because of the pointer and no menu is open.
    readonly property bool escapeWanted: !pinned && revealed && !holdOpen
    readonly property bool pointerInside: edgeHover.hovered || handleButton.hovered || barHover.hovered
    readonly property real barHeight: holder.height
    // The handle's far side from the window's left, while it is drawn: what the content under it keeps clear.
    readonly property real handleEnd: handleButton.visible && !rightToLeft ? handleButton.x + handleButton.width : 0

    implicitHeight: pinned ? holder.height : (edgeShown ? edgeHeight : 0)

    // Above the content it overlays, which is later in the layout.
    z: 10

    property bool byTouch: false

    function reveal() {
        if (pinned)
            return
        leaveTimer.stop()
        enterTimer.stop()
        revealed = true
    }
    // After a key or a command the pointer may still be on the edge: it must leave and come back
    // before it slides in again.
    property bool suppressed: false
    function conceal() {
        enterTimer.stop()
        leaveTimer.stop()
        revealed = false
        byTouch = false
        suppressed = pointerInside
    }
    onPointerInsideChanged: if (!pointerInside) suppressed = false

    onPinnedChanged: {
        revealed = false
        byTouch = false
        enterTimer.stop()
        leaveTimer.stop()
    }
    Behavior on slide {
        NumberAnimation { duration: root.slideDuration; easing.type: Easing.OutCubic }
    }

    // In and out with no one holding it.
    readonly property bool leaving: revealed && !pinned && !pointerInside && !holdOpen
    onLeavingChanged: {
        if (leaving)
            leaveTimer.restart()
        else
            leaveTimer.stop()
    }
    onHoldOpenChanged: {
        // A command was chosen (or the menu closed): the bar goes, unless the pointer is on it.
        if (!holdOpen && revealed && !pinned && !pointerInside)
            conceal()
    }
    readonly property bool hoveringEdge: !pinned && !revealed && !suppressed && (edgeHover.hovered || handleButton.hovered)
    onHoveringEdgeChanged: {
        if (hoveringEdge)
            enterTimer.restart()
        else
            enterTimer.stop()
    }
    Timer {
        id: enterTimer
        interval: root.enterDelay
        onTriggered: root.reveal()
    }
    Timer {
        id: leaveTimer
        interval: root.byTouch ? root.touchDelay : root.leaveDelay
        onTriggered: root.conceal()
    }

    // The hot edge: the whole width of the window edge, in the layout (nothing overlays the
    // canvas but the handle).
    Rectangle {
        id: edge
        objectName: "autoHideEdge"
        visible: !root.pinned && root.edgeShown
        width: parent.width
        height: root.edgeHeight
        color: root.look.well
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: root.look.hairline
        }
        HoverHandler { id: edgeHover }
        TapHandler {
            gesturePolicy: TapHandler.WithinBounds
            onTapped: {
                root.byTouch = true
                root.reveal()
            }
        }
    }

    // The handle: always visible, 28 px or more, so no hidden knowledge is needed.
    AbstractButton {
        id: handleButton
        objectName: "autoHideHandle"
        visible: !root.pinned && root.edgeShown
        opacity: 1 - root.slide
        x: root.rightToLeft ? root.width - width - 1.5 * root.u : root.handleInset + 1.5 * root.u
        y: root.edgeHeight
        width: Math.max(28, 9 * root.u)
        height: Math.max(28, 7 * root.u)
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        Accessible.name: root.handleName
        Accessible.role: Accessible.Button
        Accessible.onPressAction: root.reveal()
        onPressed: {
            root.byTouch = !hovered
            root.reveal()
        }
        background: Rectangle {
            color: handleButton.hovered ? Qt.tint(root.look.well, Qt.alpha(root.look.text, 0.08)) : root.look.well
            border.color: root.look.hairline
            radius: Math.min(root.look.corner, 6)
            // Square the top: it hangs from the edge.
            Rectangle {
                width: parent.width
                height: parent.radius
                color: parent.color
                border.width: 0
                visible: parent.radius > 0
            }
        }
        contentItem: Item {
            Column {
                anchors.centerIn: parent
                spacing: 0.75 * root.u
                Repeater {
                    model: 3
                    Rectangle {
                        width: 4 * root.u
                        height: Math.max(1, Math.round(root.u / 2))
                        color: root.look.text
                    }
                }
            }
        }
    }

    // The bar slides on this item: its y moves, the layout does not.
    Item {
        id: holder
        objectName: "autoHideBar"
        width: parent.width
        height: childrenRect.height
        y: -(1 - root.slide) * (height + 1)
        visible: root.inWindow || root.slide > 0
        HoverHandler { id: barHover; enabled: !root.pinned }
    }
}
