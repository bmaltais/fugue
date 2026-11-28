pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "KitTone.js" as KitTone

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
    // Frosted glass for a chip over a picture (KitFrost): the `backdrop` item behind it blurred, a light tint and hairline, white
    // text; without a backdrop, or on the software renderer, the tint alone.
    property bool frosted: false
    property Item backdrop: null
    property url backdropSource // the picture the backdrop shows, when it is one: a new one is measured again
    property real maxTextWidth: 0 // more than 0: a longer text is cut with an ellipsis (a project name in the window bar)
    property bool round: false // as wide as tall, fully round (a glyph button in a corner)
    property bool bordered: false // a hairline edge, for a chip that stands on its own (Cancel)
    // A filter that is on takes the window's tone (the KitPanel above, KitTone.js) as translucent colour, as a toggle does, instead of
    // the quiet selection color; with the Colour slider at 0, or no panel above, the selection color.
    property bool toned: false
    // The surface's one primary action (Install, Download all, Run): the accent fill and its ink. One per surface (docs/ui-ergonomics.md D7).
    property bool primary: false
    // A removal or a cancel of work: the red tint while it can act (docs/canvas-look.md 11).
    property bool danger: false
    // Looks disabled (dimmed) but still hovers, so its `tip` can say why it does nothing (a Move up on the first row); `tapped` is the host's to ignore.
    property bool dimmed: false
    // Its place in a dialog's KitDialogButtons row: "extra", "cancel", "destructive" or "accept" (the default).
    property string dialogRole
    // In a dialog's row (KitDialogButtons sets it): the dialog button's one size, as ViewButton's in a row (7u tall, 3u each side, the
    // text size in Medium), and a destructive answer is the solid red (`dangerSolid`, white ink) instead of the tint.
    property bool dialogRow: false
    readonly property string tone: toned ? KitTone.nearest(chip) : ""
    readonly property color currentFill: tone === "" || look.params.controlColour === "accent" ? look.selectedRow
                                         : Qt.alpha(look.tone(tone).glass, 0.34)
    // False for a label that only looks like a chip (a badge with a tooltip and nothing to press): it takes no press, so the card
    // under it gets the tap, no focus stop, no hover fill, no pointing hand; the tooltip stays.
    property bool interactive: true
    signal tapped

    readonly property bool hovered: interactive && hover.hovered
    readonly property bool pressed: tap.pressed

    implicitHeight: 7 * look.unit
    implicitWidth: round ? implicitHeight : pill && text === "" ? 2 * implicitHeight
                   : Math.max(content.implicitWidth + (dialogRow ? 6 : 5) * look.unit, implicitHeight)
    width: implicitWidth
    height: implicitHeight
    radius: round || (pill && look.params.chipShape === "pill") ? height / 2 : Math.min(look.params.cornerChip * look.unit, 0.45 * height)
    readonly property bool glass: frosted && look.frost > 0.001 // the frost setting at 0 makes it the plain chip
    readonly property color ink: primary ? look.accentForeground
                                 : solidDanger ? look.dangerInk
                                 : glass && glassLoader.item ? (glassLoader.item as KitFrost).ink : glass ? "white" : look.text
    readonly property bool darkInk: glass && glassLoader.item ? (glassLoader.item as KitFrost).pale : false
    readonly property bool alarmed: danger && enabled && !primary
    readonly property bool solidDanger: alarmed && dialogRow
    color: primary ? (pressed ? Qt.darker(look.accent, 1.15) : hovered ? Qt.lighter(look.accent, 1.08) : look.accent)
         : solidDanger ? (hovered || pressed ? look.dangerSolidHover : look.dangerSolid)
         : alarmed ? (hovered || pressed ? look.dangerHover : look.dangerFill)
         : glass ? "transparent" : pressed ? look.menuRow : current ? currentFill : hovered ? look.kitChipHover : look.kitChip
    readonly property bool ring: activeFocus && FocusInput.keyboard // the accent ring is for the keyboard only (owner, 2026-11-25)
    // In a dialog's row the primary and the solid red answer have no ring of their own: the row draws one 3px outside them
    // (KitDialogButtons; review, 2026-11-27).
    readonly property bool ringOutside: dialogRow && (primary || solidDanger)
    border.width: ring && !ringOutside ? 2 : bordered && !solidDanger ? 1 : 0
    border.color: ring ? (primary ? look.accentForeground : alarmed ? look.error : look.accent) : look.hairlineStrong
    activeFocusOnTab: enabled && interactive
    opacity: enabled && !dimmed ? 1 : 0.4
    Accessible.role: Accessible.Button
    Accessible.name: text !== "" ? text : tip
    Accessible.onPressAction: chip.tapped()
    Behavior on color { ColorAnimation { duration: chip.look.params.durColor; easing.type: Easing.OutCubic } }

    KitRaise { look: chip.look; radius: chip.radius; visible: !chip.glass; under: chip.primary || chip.solidDanger }
    Loader {
        id: glassLoader
        anchors.fill: parent
        active: chip.glass
        z: -1
        sourceComponent: KitFrost {
            look: chip.look
            backdrop: chip.backdrop
            backdropSource: chip.backdropSource
            hovered: chip.hovered
            on: chip.current
            pressed: chip.pressed
            radius: chip.radius
        }
    }
    Row {
        id: content
        anchors.centerIn: parent
        spacing: 1.75 * chip.look.unit
        Glyph {
            visible: chip.glyph !== ""
            anchors.verticalCenter: parent.verticalCenter
            look: chip.look
            name: chip.glyph
            color: chip.ink
        }
        Text {
            visible: chip.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: chip.text
            textFormat: Text.PlainText
            width: chip.maxTextWidth > 0 ? Math.min(implicitWidth, chip.maxTextWidth) : implicitWidth
            elide: Text.ElideRight
            color: chip.ink
            style: chip.glass && !chip.darkInk ? Text.Raised : Text.Normal // white ink gets a soft 1px shadow
            styleColor: Qt.rgba(0, 0, 0, 0.35)
            font.pixelSize: chip.dialogRow ? chip.look.textSize : Math.round(chip.look.textSize * 13 / 12)
            font.weight: Font.Medium
        }
    }
    TapHandler {
        id: tap
        gesturePolicy: TapHandler.WithinBounds
        enabled: chip.interactive
        onTapped: chip.tapped()
    }
    HoverHandler {
        id: hover
        cursorShape: chip.enabled && chip.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    ToolTip.visible: hover.hovered && chip.tip !== "" && chip.visible
    ToolTip.delay: chip.look.tooltipDelay
    ToolTip.text: chip.tip
    Keys.enabled: chip.interactive
    Keys.onReturnPressed: chip.tapped()
    Keys.onEnterPressed: chip.tapped()
    Keys.onSpacePressed: chip.tapped()
}
