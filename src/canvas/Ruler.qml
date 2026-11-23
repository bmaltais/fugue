pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// A ruler along one edge of a picture (#253): ticks and numbers in the picture's own pixels, following the viewer's zoom
// and pan. It knows nothing of a canvas, a guide or the viewer: give it where pixel 0 is in this item's parent
// (`origin`, along the ruler's axis), how many view units one pixel is (`zoom`) and how many pixels the picture has along
// the axis (`extent`); it draws the ticks of the part of the picture the ruler covers, with a number on every major
// tick, the step chosen so numbers are never closer than `labelGap` view units. A drag that starts on it is how a
// guide is pulled out of it: `dragStarted`, `dragMoved` and `dragEnded` say where the pointer is, in the parent's
// coordinates; the ruler itself never edits anything.
//
// Input:  look, vertical (false: along the top, measuring x; true: down the left side, measuring y), origin, zoom, extent,
//         thickness.
// Output: dragStarted(x, y), dragMoved(x, y), dragEnded(x, y) (the pointer, in the parent's coordinates),
//         step (the major tick's step in pixels).
Item {
    id: ruler
    required property CanvasLook look
    property bool vertical: false
    property real origin: 0
    property real zoom: 1
    property real extent: 0
    property real thickness: 20
    property real labelGap: 56

    signal dragStarted(real x, real y)
    signal dragMoved(real x, real y)
    signal dragEnded(real x, real y)

    // The major step: the smallest of 1, 2, 5, 10, 20, 50... pixels at least `labelGap` view units wide.
    readonly property real step: {
        for (let decade = 1; decade < 1e9; decade *= 10) {
            for (const factor of [1, 2, 5]) {
                if (decade * factor * zoom >= labelGap)
                    return decade * factor
            }
        }
        return 1e9
    }
    // Minor ticks per major: 1 -> 10 parts (a step of 1 has none), 2 -> 4, 5 -> 5.
    readonly property int parts: step < 2 ? 1 : step < 5 ? 2 : (String(step)[0] === "1" ? 10 : String(step)[0] === "2" ? 4 : 5)
    readonly property real minor: step / parts
    readonly property real along: vertical ? height : width
    readonly property real first: Math.max(0, Math.ceil(-origin / zoom / minor - 1e-9))
    readonly property real last: Math.min(Math.floor(extent / minor + 1e-9), Math.floor((along - origin) / zoom / minor + 1e-9))
    readonly property int count: last >= first ? Math.min(2000, last - first + 1) : 0

    implicitWidth: vertical ? thickness : 100
    implicitHeight: vertical ? 100 : thickness
    clip: true
    Accessible.role: Accessible.Graphic
    Accessible.name: vertical ? qsTr("Vertical ruler") : qsTr("Horizontal ruler")

    Rectangle {
        anchors.fill: parent
        color: Qt.alpha(ruler.look.popup, 0.92)
    }
    // The inner edge, a hairline, so the ruler reads as an edge of the view.
    Rectangle {
        x: ruler.vertical ? ruler.width - 1 : 0
        y: ruler.vertical ? 0 : ruler.height - 1
        width: ruler.vertical ? 1 : ruler.width
        height: ruler.vertical ? ruler.height : 1
        color: ruler.look.hairlineStrong
    }
    Repeater {
        model: ruler.count
        delegate: Item {
            id: tick
            required property int index
            readonly property real value: (ruler.first + index) * ruler.minor
            readonly property bool major: Math.abs(value / ruler.step - Math.round(value / ruler.step)) < 1e-6
            readonly property real at: ruler.origin + value * ruler.zoom
            Rectangle {
                x: ruler.vertical ? ruler.width - width : tick.at
                y: ruler.vertical ? tick.at : ruler.height - height
                width: ruler.vertical ? (tick.major ? ruler.thickness * 0.5 : ruler.thickness * 0.25) : 1
                height: ruler.vertical ? 1 : (tick.major ? ruler.thickness * 0.5 : ruler.thickness * 0.25)
                color: ruler.look.label
            }
            Text {
                visible: tick.major
                text: String(Math.round(tick.value))
                color: ruler.look.label
                font.pixelSize: ruler.look.captionSize
                font.features: { "tnum": 1 }
                // Horizontal: after the tick; vertical: turned to read upward, along the tick.
                x: ruler.vertical ? 1 : tick.at + 3
                y: ruler.vertical ? tick.at + width + 3 : 1
                rotation: ruler.vertical ? -90 : 0
                transformOrigin: Item.TopLeft
            }
        }
    }
    MouseArea {
        id: pull
        objectName: "rulerPull"
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.PointingHandCursor
        function inParent(mouse) { return mapToItem(ruler.parent, mouse.x, mouse.y) }
        onPressed: mouse => {
            const p = inParent(mouse)
            ruler.dragStarted(p.x, p.y)
        }
        onPositionChanged: mouse => {
            if (pressed) {
                const p = inParent(mouse)
                ruler.dragMoved(p.x, p.y)
            }
        }
        onReleased: mouse => {
            const p = inParent(mouse)
            ruler.dragEnded(p.x, p.y)
        }
    }
}
