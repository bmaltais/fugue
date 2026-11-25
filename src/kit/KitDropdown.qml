import QtQuick
import Fugue
import "../canvas"

// A dropdown of the Drift kit (docs/canvas-look.md "Drift kit", Dropdown): a raised chip, because it is clicked (the kit's rule:
// clickable is raised, fillable is recessed), with the chosen `text` at the start and a chevron at the end, each 2.5u from its
// edge as a KitChip's content is. It draws and reports; the owner keeps the menu (a WidgetMenu child) and opens it on `tapped`.
// `open` while that menu shows gives the chip its pressed fill. A press, Enter, Space or Down taps it. The focus ring is for the
// keyboard only (KitSegmented). Used by the App view's picture, choice and model inputs (owner, 2026-11-25: the picture picker
// was flat, and its chevron 1u from a pill's round end).
Rectangle {
    id: dd
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property string text
    property color ink: look.text // the text's colour: `label` for a prompt such as "Choose a picture", `error` for a problem
    property bool open: false
    property int elide: Text.ElideMiddle
    property bool pill: true
    signal tapped

    readonly property real pad: 2.5 * look.unit
    property bool pressFocus: false
    onActiveFocusChanged: if (!activeFocus) pressFocus = false
    readonly property bool ring: activeFocus && !pressFocus

    implicitHeight: 7 * look.unit
    implicitWidth: Math.max(14 * look.unit, label.implicitWidth + look.unit + chevron.implicitWidth + 2 * pad)
    radius: pill && look.params.chipShape === "pill" ? height / 2 : Math.min(look.params.cornerChip * look.unit, 0.45 * height)
    color: open || tap.pressed ? look.menuRow : hover.hovered ? look.kitChipHover : look.kitChip
    Behavior on color { ColorAnimation { duration: dd.look.params.durColor; easing.type: Easing.OutCubic } }
    border.width: ring || (open && activeFocus) ? 2 : 0
    border.color: look.accent
    activeFocusOnTab: enabled
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.ComboBox
    Accessible.description: text
    Accessible.onPressAction: dd.tapped()
    Keys.onReturnPressed: dd.tapped()
    Keys.onEnterPressed: dd.tapped()
    Keys.onSpacePressed: dd.tapped()
    Keys.onDownPressed: dd.tapped()

    KitRaise { look: dd.look; radius: dd.radius }
    Text {
        id: label
        anchors { left: parent.left; leftMargin: dd.pad; right: chevron.left; rightMargin: dd.look.unit; verticalCenter: parent.verticalCenter }
        text: dd.text
        textFormat: Text.PlainText
        elide: dd.elide
        color: dd.ink
        font.pixelSize: dd.look.textSize
    }
    Glyph {
        id: chevron
        anchors { right: parent.right; rightMargin: dd.pad; verticalCenter: parent.verticalCenter }
        look: dd.look
        name: "chevron-down"
        color: dd.look.label
    }
    HoverHandler { id: hover; cursorShape: dd.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler {
        id: tap
        gesturePolicy: TapHandler.WithinBounds
        onTapped: {
            if (!dd.activeFocus)
                dd.pressFocus = true
            dd.forceActiveFocus(Qt.MouseFocusReason)
            dd.tapped()
        }
    }
}
