import QtQuick
import Fugue

// A handle that moves a floating box anywhere inside an area (#120): by a drag, or by the arrow keys
// once it has the focus (Shift: five times the step). It knows nothing of the box or the canvas:
// the caller gives the size of the area and of the box, the remembered `place`, and puts the box at
// `position`. A drag follows the pointer, is pulled lightly to the area's edges and middle
// (`snapDistance`), and is kept inside, `margin` from the edges. `placeRequested` hands back the
// place as fractions of the room the box has (0 to 1 on each axis, src/engine/float_place.h), which
// the caller keeps, so a resized area keeps the box where it was relative to the edges.
//
// Its children are the handle's look (the grip's dots). The caller names it for assistive tools.
Item {
    id: grip
    property real areaWidth: 0
    property real areaHeight: 0
    property real boxWidth: 0
    property real boxHeight: 0
    property real margin: 0
    property point place: Qt.point(0.5, 0)  // remembered: fractions of the room, 0 to 1
    property real step: 8                   // an arrow key moves this far (Shift: five times)
    property real snapDistance: 12          // a drag within this of an edge or the middle is pulled to it

    readonly property bool dragging: handler.active
    // Where the box is now: its remembered place, or the pointer's while it is dragged.
    readonly property point position: handler.active ? dragged : resting

    signal placeRequested(point place)

    readonly property size areaSize: Qt.size(areaWidth, areaHeight)
    readonly property size boxSize: Qt.size(boxWidth, boxHeight)
    readonly property point resting: FloatPlace.fromFraction(place, areaSize, boxSize, margin)
    property point start: Qt.point(0, 0)
    readonly property point dragged: FloatPlace.snapped(
        FloatPlace.clamped(Qt.point(start.x + handler.activeTranslation.x, start.y + handler.activeTranslation.y), areaSize, boxSize, margin),
        areaSize, boxSize, margin, snapDistance)

    function moveBy(dx, dy) {
        const to = FloatPlace.moved(position, dx, dy, areaSize, boxSize, margin)
        placeRequested(FloatPlace.toFraction(to, areaSize, boxSize, margin))
    }

    activeFocusOnTab: true

    DragHandler {
        id: handler
        target: null
        cursorShape: active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        onActiveChanged: {
            if (active)
                grip.start = grip.resting
            else
                grip.placeRequested(FloatPlace.toFraction(grip.dragged, grip.areaSize, grip.boxSize, grip.margin))
        }
    }
    Keys.onPressed: event => {
        const far = (event.modifiers & Qt.ShiftModifier) ? 5 * step : step
        if (event.key === Qt.Key_Left)
            moveBy(-far, 0)
        else if (event.key === Qt.Key_Right)
            moveBy(far, 0)
        else if (event.key === Qt.Key_Up)
            moveBy(0, -far)
        else if (event.key === Qt.Key_Down)
            moveBy(0, far)
        else
            return
        event.accepted = true
    }
}
