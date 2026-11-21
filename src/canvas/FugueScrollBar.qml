import QtQuick
import QtQuick.Controls
import Fugue

// The scroll bar of every list and panel that can overflow: attach it as
// `ScrollBar.vertical: FugueScrollBar { look: ... }` (or `.horizontal`) on a ListView, GridView,
// Flickable or ScrollView. It shows while the content is longer than the view, always (not only
// on hover), and hides when it fits. It lies over the view's edge, so every host keeps its
// content clear of it by `inset` (the bar's breadth while it shows, else 0). The thumb never
// gets shorter than `minThumb`, so a list of hundreds of rows keeps a thumb to grab, and a press
// on the track outside the thumb moves one page toward it.
ScrollBar {
    id: bar
    objectName: "fugueScrollBar"
    required property CanvasLook look // App.theme.look

    // The shortest thumb, in px, along the bar.
    property int minThumb: 32
    // Breadth of the bar; the thumb is drawn narrower inside it.
    readonly property int breadth: 10
    // The room the bar takes from the view's edge while it shows, else 0. The bar lies over the
    // view, so a host pads its content by this (`list.ScrollBar.vertical.inset`): no row, key chip
    // or button ever sits under the bar.
    readonly property real inset: visible ? (vertical ? width : height) : 0

    policy: size < 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
    minimumSize: (vertical ? height : width) > 0 ? Math.min(1, minThumb / (vertical ? height : width)) : 0
    // Below the 24px floor on purpose: the bar is a gutter along an edge, 10px wide, and
    // its press target is the whole gutter; wheel and keys remain the main way to scroll.
    implicitWidth: vertical ? breadth : 0
    implicitHeight: vertical ? 0 : breadth
    padding: 0

    contentItem: Rectangle {
        implicitWidth: bar.vertical ? 6 : bar.minThumb
        implicitHeight: bar.vertical ? bar.minThumb : 6
        radius: 3
        color: bar.look.label
        opacity: bar.pressed ? 0.9 : bar.hovered ? 0.7 : 0.45
    }
    background: Rectangle {
        color: bar.look.well
        opacity: 0.6
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
