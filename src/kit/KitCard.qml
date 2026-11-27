pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// A card of the Drift kit that can be pressed (docs/canvas-look.md 20.19): an app on the shelf, a template in the Templates window.
// Clickable is raised, so it has the light top edge and the soft shadow under (KitRaise); it is a step off the panel it
// sits on (the group's glass fill, twice as strong on a dark theme so it stands out of the tint), lighter again under the pointer, the 2px
// accent ring when it has the keyboard's place (`current`). The corner is the group's, so the Corners slider moves it; the
// Flat preset gives square corners and a plain fill with the same code. Children go inside it. `tapped` says it was pressed;
// a control inside that takes its own press (a chip) keeps the tap from the card.
Rectangle {
    id: card
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property bool current: false
    default property alias content: holder.data
    signal tapped
    readonly property bool hovered: hover.hovered

    readonly property bool glass: look.params.panelTint > 0.001
    readonly property color fill: glass ? (look.light ? Qt.rgba(0, 0, 0, look.params.groupFillAlphaLight)
                                                       : Qt.rgba(1, 1, 1, 2 * look.params.groupFillAlpha))
                                        : look.kitGroup
    radius: Math.min(look.params.cornerGroup * look.unit, 0.2 * Math.min(width, height))
    color: hovered ? Qt.tint(fill, look.light ? Qt.rgba(0, 0, 0, 0.035) : Qt.rgba(1, 1, 1, 0.045)) : fill
    border.width: current ? 2 : look.params.borders === "hairline" ? 1 : 0
    border.color: current ? look.accent : look.light ? Qt.rgba(0, 0, 0, 0.08) : Qt.rgba(1, 1, 1, 0.07)
    Behavior on color { ColorAnimation { duration: card.look.params.durColor; easing.type: Easing.OutCubic } }

    KitRaise { look: card.look; radius: card.radius; outsideOnly: card.glass }
    Item { id: holder; anchors.fill: parent }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    // WithinBounds takes the press for this card alone: a passive grab (the default DragThreshold) lets the press go on to
    // a handler under the card too, through a modal popup, so a Templates card also opened the shelf's app under it.
    TapHandler { gesturePolicy: TapHandler.WithinBounds; onTapped: card.tapped() }
}
