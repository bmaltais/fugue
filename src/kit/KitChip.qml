import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"

// A small rounded button of the Drift kit (docs/canvas-look.md "Drift kit"): the `kitChip` fill, a Lucide `glyph`
// before the `text` or alone, 7u tall (the floor). `pill` rounds it fully, as the footer's pause and shuffle buttons are;
// a pill with only a glyph is twice as wide as tall. `current` marks a toggle that is on, in the quiet selection
// color (never the accent: the surface's one primary stays the only filled accent, docs/ui-ergonomics.md D7).
// Hover is the `kitChipHover` fill, focus the 2px accent ring; Tab reaches it, Enter and Space press it. A button
// with no text names itself through `tip`, also its tooltip. It knows nothing of what it does: `tapped` says it
// was pressed.
Rectangle {
    id: chip
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property string text
    property string glyph
    property string tip
    property bool pill: false
    property bool current: false
    signal tapped

    readonly property bool hovered: hover.hovered
    readonly property bool pressed: tap.pressed

    implicitHeight: 7 * look.unit
    implicitWidth: pill && text === "" ? 2 * implicitHeight : Math.max(content.implicitWidth + 5 * look.unit, implicitHeight)
    width: implicitWidth
    height: implicitHeight
    radius: pill ? height / 2 : look.smallCorner + 2
    color: pressed ? look.menuRow : current ? look.selectedRow : hovered ? look.kitChipHover : look.kitChip
    border.width: activeFocus ? 2 : 0
    border.color: look.accent
    activeFocusOnTab: enabled
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.Button
    Accessible.name: text !== "" ? text : tip
    Accessible.onPressAction: chip.tapped()
    Behavior on color { ColorAnimation { duration: 100; easing.type: Easing.OutCubic } }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 1.75 * chip.look.unit
        Glyph {
            visible: chip.glyph !== ""
            anchors.verticalCenter: parent.verticalCenter
            look: chip.look
            name: chip.glyph
            color: chip.look.text
        }
        Text {
            visible: chip.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: chip.text
            textFormat: Text.PlainText
            color: chip.look.text
            font.pixelSize: Math.round(chip.look.textSize * 13 / 12)
            font.weight: Font.Medium
        }
    }
    TapHandler {
        id: tap
        gesturePolicy: TapHandler.WithinBounds
        onTapped: chip.tapped()
    }
    HoverHandler {
        id: hover
        cursorShape: chip.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    ToolTip.visible: hover.hovered && chip.tip !== "" && chip.visible
    ToolTip.delay: chip.look.tooltipDelay
    ToolTip.text: chip.tip
    Keys.onReturnPressed: chip.tapped()
    Keys.onEnterPressed: chip.tapped()
    Keys.onSpacePressed: chip.tapped()
}
