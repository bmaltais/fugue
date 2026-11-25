import QtQuick
import QtQuick.Controls
import Fugue

// The scroll bar of every list and panel that can overflow: attach it as
// `ScrollBar.vertical: FugueScrollBar { look: ... }` (or `.horizontal`) on a ListView, GridView,
// Flickable or ScrollView. It shows while the content is longer than the view, always (not only
// on hover), and hides when it fits. It lies over the view's edge, so every host keeps its
// content clear of it by `inset` (the bar's breadth while it shows, else 0), unless the host's
// own padding on that side holds the bar. The thumb never
// gets shorter than `minThumb`, so a list of hundreds of rows keeps a thumb to grab, and a press
// on the track outside the thumb moves one page toward it. It also gives its view the mouse wheel
// (below): Qt's own wheel handling of a Flickable does not hear the wheel as the owner's Wayland
// session sends it (a device of type TouchPad, no scroll phase, an angle of 120 and a pixel delta
// of 15 per notch), so the wheel did not scroll any list or panel.
ScrollBar {
    id: bar
    objectName: "fugueScrollBar"
    required property CanvasLook look // App.theme.look

    readonly property real u: look.unit
    // The shortest thumb, in px, along the bar: 8u.
    property int minThumb: Math.round(8 * u)
    // Breadth of the gutter (2.5u), the whole of which is the press target; the thumb is drawn
    // narrower inside it (1u, 1.5u while hovered or pressed).
    readonly property int breadth: Math.round(2.5 * u)
    // The host's padding on the bar's side (a ScrollView's `rightPadding`, or `leftPadding` mirrored;
    // `bottomPadding` for a horizontal bar), 0 for a host without one. A padding of at least the
    // gutter holds the bar: it lies in the padding and the content does not shrink.
    readonly property real hostPadding: {
        const v = bar.parent as ScrollView
        if (!v)
            return 0
        const p = bar.vertical ? (bar.mirrored ? v.leftPadding : v.rightPadding) : v.bottomPadding
        return p === undefined ? 0 : p
    }
    // The room the bar takes from the content while it shows, else 0 (also 0 when the host's
    // padding holds it). Otherwise the bar lies over the view, so a host pads its content by this
    // (`list.ScrollBar.vertical.inset`): no row, key chip or button ever sits under the bar.
    readonly property real inset: visible && hostPadding < breadth ? (vertical ? width : height) : 0

    policy: size < 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
    minimumSize: (vertical ? height : width) > 0 ? Math.min(1, minThumb / (vertical ? height : width)) : 0
    // Below the 24px floor on purpose: the bar is a gutter along an edge, and its press target is
    // the whole gutter; wheel and keys remain the main way to scroll.
    implicitWidth: vertical ? breadth : 0
    implicitHeight: vertical ? 0 : breadth
    padding: 0
    focusPolicy: Qt.NoFocus
    hoverEnabled: true

    // The wheel over the whole view, not only the bar. A notch moves the view 60px; a touchpad,
    // told by its scroll phase alone, moves it by its own pixels. The view's own wheel handling
    // never sees the event (this handler takes it), so a view does not scroll twice.
    // Only a view that can scroll takes the wheel: one whose content fits lets the event through to
    // what is behind it (the canvas zooms under a field that does not overflow).
    readonly property bool wheelScrolls: {
        const view = bar.parent as Flickable
        if (!view)
            return false
        return bar.vertical ? view.contentHeight > view.height - view.topMargin - view.bottomMargin
                            : view.contentWidth > view.width - view.leftMargin - view.rightMargin
    }
    WheelHandler {
        parent: bar.parent
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        blocking: bar.wheelScrolls
        onWheel: event => {
            const view = bar.parent as Flickable
            if (!view)
                return
            const touchpad = event.phase !== Qt.NoScrollPhase
            const dy = touchpad && event.pixelDelta.y !== 0 ? event.pixelDelta.y : event.angleDelta.y / 2
            const dx = touchpad && event.pixelDelta.x !== 0 ? event.pixelDelta.x : event.angleDelta.x / 2
            // The bar's own axis; a horizontal bar also takes the vertical wheel (a filmstrip).
            const by = bar.vertical ? dy : (dx !== 0 ? dx : dy)
            if (by === 0)
                return
            if (bar.vertical) {
                const low = view.originY - view.topMargin
                const high = view.originY + view.contentHeight - view.height + view.bottomMargin
                if (high > low)
                    view.contentY = Math.max(low, Math.min(high, view.contentY - by))
            } else {
                const low = view.originX - view.leftMargin
                const high = view.originX + view.contentWidth - view.width + view.rightMargin
                if (high > low)
                    view.contentX = Math.max(low, Math.min(high, view.contentX - by))
            }
        }
    }

    // The thumb: a round pill in `label`, centred in the gutter, thin at rest and thicker (and
    // stronger) while the pointer is on the gutter or presses it. It animates over durColor (0
    // under reduced motion).
    contentItem: Item {
        implicitWidth: bar.vertical ? bar.breadth : bar.minThumb
        implicitHeight: bar.vertical ? bar.minThumb : bar.breadth
        Rectangle {
            property real thick: (bar.hovered || bar.pressed ? 1.5 : 1) * bar.u
            width: bar.vertical ? thick : parent.width
            height: bar.vertical ? parent.height : thick
            x: bar.vertical ? (parent.width - width) / 2 : 0
            y: bar.vertical ? 0 : (parent.height - height) / 2
            radius: thick / 2
            color: Qt.alpha(bar.look.label, bar.pressed ? bar.look.params.thumbPress
                                                         : bar.hovered ? bar.look.params.thumbHover
                                                                       : bar.look.light ? bar.look.params.thumbAlphaLight : bar.look.params.thumbAlpha)
            Behavior on thick { NumberAnimation { duration: bar.look.params.durColor } }
            Behavior on color { ColorAnimation { duration: bar.look.params.durColor } }
        }
    }
    // No track at rest; the recessed `kitTrack`, rounded, while hovered or pressed.
    background: Rectangle {
        radius: (bar.vertical ? width : height) / 2
        color: bar.look.kitTrack
        opacity: bar.hovered || bar.pressed ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: bar.look.params.durColor } }
        MouseArea {
            // A press on the track pages toward it; one on the thumb is the bar's own drag.
            anchors.fill: parent
            onPressed: mouse => {
                const along = bar.vertical ? mouse.y : mouse.x
                const total = bar.vertical ? height : width
                const start = bar.position * total
                const end = (bar.position + bar.size) * total
                if (along >= start && along <= end) {
                    mouse.accepted = false
                    return
                }
                const page = bar.size
                bar.position = Math.max(0, Math.min(1 - bar.size, bar.position + (along < start ? -page : page)))
            }
        }
    }
}
