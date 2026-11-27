pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../kit"
import "../kit/KitTone.js" as KitTone

// The window's one button (docs/ui-ergonomics.md D4): a large view's bar and footer (#62, #73), the run toolbar and the
// Queue area (#60), the Canvas view, and through thin aliases the settings' SmallButton (SettingsParts.qml) and the
// area header's buttons (AreaHeader.qml). 7u = 28px tall, above the 24px floor, a label on the well color.
//
// Variants, which combine:
//  - `primary`: the surface's one highlighted action (OK, Run, Accept), the accent fill. One per surface.
//  - `current`: a choice that holds or a toggle that is on. `accented` (the default) fills it with the accent, for a
//    surface with no primary; `accented: false` is the quiet marking (the selection color), which a toggle on a surface
//    with a primary always uses (D7). `checkable` tells assistive tools it is a toggle and whether it is on.
//  - `danger`: a cancel or a removal, the red tint while it can act (docs/canvas-look.md 11).
//  - `bordered`: the forms' look (settings, Projects), a hairline edge.
//  - `flat`: chrome (an area header, a panel's title): transparent until hovered, the label color at rest.
//  - `compact`: 6u = 24px, for a bar that is itself 7u tall.
// `glyph` (a Lucide name, docs/canvas-look.md 16) puts an icon before the label, or alone; `trailingGlyph` one after it
// (`chevron-down` for a button that opens a menu). A button without a label names itself through `tip`, which is also
// its tooltip. States as every button draws them: hover (an edge, or the menu-row fill when flat), pressed, the 2px
// accent focus ring (the error color on a danger button), disabled at 40%.
// On the Drift kit (docs/canvas-look.md 20.21): a button that acts is raised (KitRaise: a light top edge, a soft shadow under) on the
// kit's chip fill, with the kit's chip corner, and a choice that holds (`current`) inside a KitPanel is the panel's tone as
// translucent colour, as KitChip.toned is; with no panel above, or Colour at 0, the accent or the selection colour as before. It takes Tab focus; Enter and Space press it.
// It knows nothing of what it does: `tapped` says it was pressed.
Rectangle {
    id: button
    required property CanvasLook look
    property string text
    property string glyph
    property string trailingGlyph
    property string tip
    property bool current: false
    property bool accented: true  // `current` shows in the accent; false in the quiet selection color
    property bool primary: false
    property bool danger: false
    property bool bordered: false
    property bool flat: false
    property bool compact: false
    // A toggle (Compare, Snap, Show candidate): assistive tools hear it as on or off, `current` being on.
    property bool checkable: false
    signal tapped
    // An Alt+click of its own (#226: a layer's eye shows that layer alone); with it, a plain click is a click with no
    // modifier.
    property bool altAction: false
    signal altTapped

    readonly property string tone: KitTone.nearest(button)
    readonly property bool toned: current && tone !== "" && look.params.controlColour !== "accent" && look.params.panelTint > 0.001
    readonly property bool raised: !flat && !marked && !alarmed && !toned
    readonly property bool hovered: hover.hovered
    readonly property bool pressed: tap.pressed
    readonly property bool marked: primary || (current && accented && !toned)
    readonly property bool alarmed: danger && enabled && !marked
    readonly property color ink: marked ? look.accentForeground
                                : flat && !current && !hovered && !activeFocus ? look.label : look.text
    implicitWidth: Math.max(content.implicitWidth + (bordered ? 6 : 4) * look.unit, implicitHeight)
    implicitHeight: (compact ? 6 : 7) * look.unit
    width: implicitWidth
    height: implicitHeight
    radius: Math.min(look.params.cornerChip * look.unit, 0.45 * height)
    color: marked ? look.accent
         : alarmed && flat ? (hovered || pressed ? look.dangerHover : "transparent") // chrome: red only when aimed at
         : alarmed ? (hovered || pressed ? look.dangerHover : look.dangerFill)
         : pressed ? look.menuRow
         : toned ? Qt.alpha(look.tone(tone).glass, 0.34)
         : current ? look.selectedRow
         : flat ? (hovered ? look.menuRow : "transparent")
         : hovered ? look.kitChipHover : look.kitChip
    // The edge: the focus ring first, then a form button's hairline, then a plain well button's hover edge.
    readonly property bool ring: activeFocus && FocusInput.keyboard // the accent ring is for the keyboard only (owner, 2026-11-25)
    border.width: ring ? 2 : bordered && !toned ? 1 : 0
    border.color: ring ? (alarmed ? look.error : look.accent) : look.hairlineStrong
    activeFocusOnTab: enabled
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.Button
    Accessible.name: text !== "" ? text : tip
    Accessible.checkable: checkable
    Accessible.checked: checkable && current
    // Made only while the button is raised (the window keeps few items).
    Loader {
        anchors.fill: parent
        active: button.raised
        sourceComponent: KitRaise { look: button.look; radius: button.radius }
    }
    Row {
        id: content
        anchors.centerIn: parent
        spacing: 1.5 * button.look.unit
        Glyph {
            visible: button.glyph !== ""
            anchors.verticalCenter: parent.verticalCenter
            look: button.look
            name: button.glyph
            // A flat danger button (an area's close) turns its glyph red on hover or focus, as the menu bar's close.
            color: button.flat && button.danger && (button.hovered || button.activeFocus) ? button.look.error : button.ink
        }
        Text {
            id: label
            visible: button.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: button.text
            color: button.ink
            font.pixelSize: button.look.labelSize
        }
        Glyph {
            visible: button.trailingGlyph !== ""
            anchors.verticalCenter: parent.verticalCenter
            look: button.look
            name: button.trailingGlyph
            color: button.marked ? button.ink : button.look.label
        }
    }
    TapHandler {
        id: tap
        // The press is the button's own (#235): a card or row under it that also takes taps does not get this one.
        gesturePolicy: TapHandler.WithinBounds
        acceptedModifiers: button.altAction ? Qt.NoModifier : Qt.KeyboardModifierMask
        onTapped: button.tapped()
    }
    TapHandler {
        enabled: button.altAction
        acceptedModifiers: Qt.AltModifier
        onTapped: button.altTapped()
    }
    HoverHandler {
        id: hover
        cursorShape: button.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    // Qt gives a disabled item, and every handler on it, no hover (#281), so a disabled button's own handler never
    // fires and its tip ("why is this off") went unseen. While disabled, this handler watches the button's parent
    // instead (the same trick as TipGate) and takes the pointer's place in the parent's coordinates. It only watches
    // (not blocking), and clicks stay blocked: the button's TapHandler is as disabled as the button.
    HoverHandler {
        id: offHover
        parent: button.parent
        enabled: !button.enabled
        blocking: false
    }
    readonly property bool tipOver: enabled ? hover.hovered
        : offHover.hovered && offHover.point.position.x >= x && offHover.point.position.x < x + width
          && offHover.point.position.y >= y && offHover.point.position.y < y + height
    ToolTip.visible: tipOver && button.tip !== "" && button.visible
    ToolTip.delay: button.look.tooltipDelay
    ToolTip.text: button.tip
    Keys.onReturnPressed: button.tapped()
    Keys.onEnterPressed: button.tapped()
    Keys.onSpacePressed: button.tapped()
}
