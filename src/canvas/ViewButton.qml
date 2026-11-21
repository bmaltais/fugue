import QtQuick
import QtQuick.Controls
import Fugue

// A button of a large view's bar and footer (#62, #73) and of the run toolbar and queue panel (#60):
// 7u = 28px tall, above the 24px floor, a label on the well color; `current` marks the choice that
// holds (the accent, or the quiet marking for a zoom), `primary` is the surface's one highlighted
// action (OK, Run). `glyph` (a Lucide name, docs/canvas-look.md 16) puts an icon before the label, or
// alone; a button without a label names itself through `tip`, which is also its tooltip. It takes
// Tab focus, shown by a ring in the accent; Enter and Space press it. It knows nothing of what it
// does: `tapped` says it was pressed.
Rectangle {
    id: button
    required property CanvasLook look
    property string text
    property string glyph
    property string tip
    property bool current: false
    property bool accented: true  // `current` shows in the accent; false in the quiet selection color
    property bool primary: false
    // A cancel or a delete: a red tint while it can act (docs/canvas-look.md 11); disabled it is as calm as any button.
    property bool danger: false
    signal tapped

    readonly property bool marked: primary || (current && accented)
    readonly property bool alarmed: danger && enabled && !marked
    readonly property color ink: marked ? look.accentForeground : look.text
    implicitWidth: Math.max(content.implicitWidth + 4 * look.unit, implicitHeight)
    implicitHeight: 7 * look.unit
    width: implicitWidth
    height: implicitHeight
    radius: look.smallCorner
    color: marked ? look.accent
         : alarmed ? (hover.hovered || tap.pressed ? look.dangerHover : look.dangerFill)
         : current ? look.selectedRow : look.well
    border.width: activeFocus ? 2 : 0
    border.color: alarmed ? look.error : look.accent
    activeFocusOnTab: enabled
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.Button
    Accessible.name: text !== "" ? text : tip
    Row {
        id: content
        anchors.centerIn: parent
        spacing: 1.5 * button.look.unit
        Text {
            visible: button.glyph !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: button.glyph !== "" ? button.look.glyph(button.glyph) : ""
            font.family: button.look.iconFont
            font.pixelSize: 14
            color: button.ink
        }
        Text {
            id: label
            visible: button.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: button.text
            color: button.ink
            font.pixelSize: button.look.labelSize
        }
    }
    TapHandler {
        id: tap
        onTapped: button.tapped()
    }
    HoverHandler {
        id: hover
        cursorShape: button.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    ToolTip.visible: hover.hovered && button.tip !== "" && button.enabled
    ToolTip.text: button.tip
    ToolTip.delay: 600
    Keys.onReturnPressed: button.tapped()
    Keys.onSpacePressed: button.tapped()
}
