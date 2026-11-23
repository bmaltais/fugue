pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// The guides over a picture (#253): thin cyan lines across the whole item at pixel positions of the picture, following
// the viewer's zoom and pan. Each can be grabbed: drag it to move it, drag it out of `keep` (the picture's part where a
// guide may stay: the caller leaves the rulers and everything off the picture out of it) and let go to remove it. It
// edits nothing: it says `moved(index, at)` and `removeRequested(index)` and the caller writes them. `draft` is the line
// a ruler is pulling out right now ({axis, at}, or null): drawn like the others with its position in a chip.
//
// Only a narrow strip along each line takes the pointer (`grab`, 12 view units), so the frame and the tools under the
// lines work everywhere else.
//
// Input:  look, guides ([{axis: "x" | "y", at}], an "x" guide is a vertical line), imageSize, originX, originY, zoom,
//         draft, keep, active (guides can be grabbed).
// Output: moved(index, at), removeRequested(index), grabbed (a press on a guide).
Item {
    id: lines
    required property CanvasLook look
    property var guides: []
    property size imageSize: Qt.size(0, 0)
    property real originX: 0
    property real originY: 0
    property real zoom: 1
    property var draft: null
    property rect keep: Qt.rect(0, 0, 0, 0)
    property bool active: true
    readonly property real grab: 12
    // The guide being dragged: its index and where it is now (pixels); -1 for none.
    readonly property int dragged: dragState.index
    readonly property bool dragging: dragState.index >= 0

    signal moved(int index, int at)
    signal removeRequested(int index)
    signal grabbed

    clip: true

    function viewOf(axis, at) { return axis === "x" ? originX + at * zoom : originY + at * zoom }
    function pixelOf(axis, point) { return Math.round(((axis === "x" ? point.x : point.y) - (axis === "x" ? originX : originY)) / zoom) }
    function inKeep(point) { return point.x >= keep.left && point.x <= keep.right && point.y >= keep.top && point.y <= keep.bottom }

    QtObject {
        id: dragState
        property int index: -1
        property int at: 0
        property bool outside: false
    }

    // One line: a dark halo and the cyan line, over the whole item; a chip with the position while it is pulled.
    component Line: Item {
        id: line
        required property string axis
        required property int at
        property bool lifted: false // being dragged: the chip shows
        property bool outside: false // about to be removed: the line is drawn faint
        readonly property bool across: axis === "x"
        readonly property real view: lines.viewOf(axis, at)
        anchors.fill: parent
        Rectangle { x: line.across ? line.view - 1 : 0; y: line.across ? 0 : line.view - 1; width: line.across ? 3 : parent.width; height: line.across ? parent.height : 3; color: Qt.rgba(0, 0, 0, line.outside ? 0.1 : 0.35) }
        Rectangle { x: line.across ? line.view : 0; y: line.across ? 0 : line.view; width: line.across ? 1 : parent.width; height: line.across ? parent.height : 1; color: Qt.rgba(0.2, 0.85, 1, line.outside ? 0.4 : 1) }
        Rectangle {
            visible: line.lifted
            x: Math.max(0, Math.min(line.across ? line.view + lines.look.unit : lines.look.unit * 3, parent.width - width))
            y: Math.max(0, Math.min(line.across ? lines.look.unit * 3 : line.view + lines.look.unit, parent.height - height))
            width: chipText.implicitWidth + 2 * lines.look.unit
            height: chipText.implicitHeight + lines.look.unit
            radius: lines.look.smallCorner
            color: Qt.rgba(0, 0, 0, 0.7)
            Text {
                id: chipText
                anchors.centerIn: parent
                text: (line.across ? qsTr("x %1") : qsTr("y %1")).arg(line.at)
                color: "white"
                font.pixelSize: lines.look.captionSize
                font.features: { "tnum": 1 }
            }
        }
    }

    Repeater {
        model: lines.guides
        delegate: Line {
            required property var modelData
            required property int index
            objectName: "guide-" + modelData.axis + "-" + modelData.at
            axis: modelData.axis
            at: lines.dragged === index ? dragState.at : modelData.at
            lifted: lines.dragged === index
            outside: lines.dragged === index && dragState.outside
        }
    }
    Line {
        objectName: "guideDraft"
        visible: lines.draft !== null && lines.draft !== undefined
        axis: lines.draft ? lines.draft.axis : "x"
        at: lines.draft ? lines.draft.at : 0
        lifted: visible
        outside: lines.draft ? lines.draft.outside === true : false
    }
    // The strips that take a press: one narrow MouseArea along each line (not following the drag, so the pointer's
    // position stays steady while the drawn line moves).
    Repeater {
        model: lines.guides
        delegate: MouseArea {
            id: strip
            required property var modelData
            required property int index
            objectName: "guideGrab-" + modelData.axis + "-" + modelData.at
            readonly property bool across: modelData.axis === "x"
            enabled: lines.active
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            cursorShape: across ? Qt.SplitHCursor : Qt.SplitVCursor
            x: across ? lines.viewOf(modelData.axis, modelData.at) - lines.grab / 2 : 0
            y: across ? 0 : lines.viewOf(modelData.axis, modelData.at) - lines.grab / 2
            width: across ? lines.grab : lines.width
            height: across ? lines.height : lines.grab
            function point(mouse) { return mapToItem(lines, mouse.x, mouse.y) }
            onPressed: mouse => {
                dragState.index = strip.index
                dragState.at = modelData.at
                dragState.outside = false
                lines.grabbed()
            }
            onPositionChanged: mouse => {
                if (!pressed || dragState.index !== strip.index)
                    return
                const p = point(mouse)
                dragState.at = lines.pixelOf(modelData.axis, p)
                dragState.outside = !lines.inKeep(p)
            }
            onReleased: mouse => {
                if (dragState.index !== strip.index)
                    return
                const p = point(mouse)
                const at = lines.pixelOf(modelData.axis, p)
                const index = strip.index
                const gone = !lines.inKeep(p)
                dragState.index = -1
                if (gone)
                    lines.removeRequested(index)
                else if (at !== modelData.at)
                    lines.moved(index, at)
            }
            onCanceled: dragState.index = -1
        }
    }
}
