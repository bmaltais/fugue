pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// The recessed surface of a field (docs/canvas-look.md "Drift kit", the rule: clickable things are raised, fillable things are
// recessed): the `kitWell` fill a shade darker than the group it sits in, a border nearly the fill's own color, a 1px shadow along
// the top edge (about 22% black on a dark theme, 8% on a light one) and a faint light hairline along the bottom edge, so a field
// reads as "set in", not as a hole. `focused` lifts the edge to 1px of `label` at 45%
// (never the accent ring: a field shows its caret, owner 2026-11-25, same width so nothing jumps). Put it first in the field, filling it; it
// takes no input and carries no text.
Rectangle {
    id: well
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property bool focused: false
    // Over a picture the well is smoked glass (KitFrost, smoked): a dark translucent tint that deepens over a pale picture, the
    // blur, white text. Give it the `backdrop` item it sits over; `labelInk` is the ink for a label beside it (dark over a pale
    // picture). With the frost setting at 0, or no backdrop, it is the opaque recessed well.
    property Item backdrop: null
    property url backdropSource
    readonly property bool glass: backdrop !== null && look.frost > 0.001
    readonly property color ink: glassLoader.item ? (glassLoader.item as KitFrost).ink : look.text
    readonly property color labelInk: glassLoader.item ? (glassLoader.item as KitFrost).labelInk : look.label
    property bool dashed: false // the picture well draws its own dashed edge; this one then leaves the border off

    anchors.fill: parent
    radius: Math.min(look.params.cornerWell * look.unit, 0.45 * height)
    color: glass ? "transparent" : Qt.tint(look.kitGroup, Qt.rgba(0, 0, 0, look.light ? look.params.wellDarkenLight : look.params.wellDarken))
    border.width: dashed ? 0 : 1
    border.color: focused ? Qt.rgba(look.label.r, look.label.g, look.label.b, 0.45) : Qt.tint(color, Qt.rgba(0, 0, 0, look.light ? 0.03 : 0.06))
    enabled: false
    Loader {
        id: glassLoader
        anchors.fill: parent
        active: well.glass
        z: -1
        sourceComponent: KitFrost {
            look: well.look
            smoked: true
            backdrop: well.backdrop
            backdropSource: well.backdropSource
            radius: well.radius
        }
    }
    Rectangle { // the shadow along the top edge
        visible: !well.glass && height > 0.01
        x: well.radius * 0.6
        y: well.border.width
        width: parent.width - 2 * x
        height: well.look.params.wellShadowHeight
        color: Qt.rgba(0, 0, 0, well.look.light ? well.look.params.wellShadowAlphaLight : well.look.params.wellShadowAlpha)
    }
    Rectangle { // the light line along the bottom edge
        visible: !well.glass && well.look.params.wellBottomLight > 0.001
        x: well.radius * 0.6
        y: parent.height - 1 - well.border.width
        width: parent.width - 2 * x
        height: 1
        color: well.look.light ? Qt.rgba(1, 1, 1, well.look.params.wellBottomLightLight) : Qt.rgba(1, 1, 1, well.look.params.wellBottomLight)
    }
}
