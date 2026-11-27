import QtQuick
import Fugue

// The raised look of a clickable thing (docs/canvas-look.md "Drift kit", the rule: clickable things are raised, fillable things
// are recessed): a 1px light edge along the top (white at 7% on a dark theme, 60% on a light one) and a soft 1px shadow under it.
// Put it in a chip, a rail item or the Run pill, filling it, with the same `radius`; it takes no input. A frosted control has its
// own glass and does not use it.
Item {
    id: raise
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property real radius: 0
    property bool outsideOnly: false // a translucent host (a card): the shadow only below its edge, or it would darken the fill
    // The shadow under the host's fill instead of over it, for a fill whose colour must stay as it is (the accent of a primary, the solid
    // red): over it, the accent read as disabled (Get ready's Install, review 2026-11-27). Other raised controls keep the shadow over
    // their fill, which their chip colours were tuned with (without it a light theme's chips melt into their group).
    property bool under: false
    anchors.fill: parent
    enabled: false
    // The control it raises: its parent, or the Loader's parent when it is made in one (ViewButton).
    readonly property Item host: parent instanceof Loader ? parent.parent : parent
    Rectangle { // the soft shadow under the control
        // With `under`, a child of the host with a negative z, which is drawn before the host's own fill: only the offset below its edge
        // shows.
        parent: raise.under && !raise.outsideOnly && raise.host ? raise.host : raise
        x: raise.outsideOnly ? raise.radius * 0.5 : 0
        width: parent.width - 2 * x
        y: raise.outsideOnly ? parent.height - 1 : raise.look.params.raiseShadowOffset
        height: raise.outsideOnly ? 1 + 2 * raise.look.params.raiseShadowOffset : parent.height
        radius: raise.outsideOnly ? 1 : raise.radius
        color: Qt.rgba(0, 0, 0, raise.look.light ? raise.look.params.raiseShadowAlphaLight : raise.look.params.raiseShadowAlpha)
        visible: raise.visible && raise.look.params.raiseShadowOffset > 0.01
        z: -1
    }
    Rectangle { // the light top edge
        x: raise.radius * 0.6
        y: 0
        width: parent.width - 2 * x
        height: 1
        color: raise.look.light ? Qt.rgba(1, 1, 1, raise.look.params.raiseEdgeLight) : Qt.rgba(1, 1, 1, raise.look.params.raiseEdge)
    }
}
