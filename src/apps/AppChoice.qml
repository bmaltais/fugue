import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "../canvas/widgets"
import "../kit"

// A choice of the app panel in the kit's chip look: a KitChip-sized pill with the chosen word and a chevron, opening the window's
// menu of the options (arrows move, Enter picks, Escape closes). `options` are {value, text}; `value` is the chosen one;
// `picked(value)` says the person chose, it never sets `value` itself.
Rectangle {
    id: chip
    property CanvasLook look: App.theme.look
    property var options: []
    property var value
    property string label // the screen reader's name
    signal picked(var value)

    readonly property string shownText: {
        for (const o of options)
            if (o.value === value)
                return o.text
        return value === undefined || value === null ? "" : String(value)
    }
    implicitHeight: 7 * look.unit
    implicitWidth: Math.max(content.implicitWidth + 4 * look.unit, 14 * look.unit)
    radius: height / 2
    color: menu.opened ? look.selectedRow : hover.hovered ? look.kitChipHover : look.kitChip
    border.width: activeFocus ? 2 : 0
    border.color: look.accent
    activeFocusOnTab: enabled
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.ComboBox
    Accessible.name: label
    Accessible.description: shownText
    Keys.onReturnPressed: chip.show()
    Keys.onEnterPressed: chip.show()
    Keys.onSpacePressed: chip.show()
    Keys.onDownPressed: chip.show()
    function show() {
        const at = chip.mapToItem(Overlay.overlay, 0, chip.height)
        menu.wantX = at.x
        menu.wantY = at.y
        menu.anchorHeight = chip.height
        menu.open()
    }
    Row {
        id: content
        anchors.centerIn: parent
        spacing: chip.look.unit
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: chip.shownText
            textFormat: Text.PlainText
            color: chip.look.text
            font.pixelSize: Math.round(chip.look.textSize * 13 / 12)
            font.weight: Font.Medium
        }
        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            look: chip.look
            name: "chevron-down"
            color: chip.look.label
            font.pixelSize: 12
        }
    }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { gesturePolicy: TapHandler.WithinBounds; onTapped: { chip.forceActiveFocus(); chip.show() } }
    WidgetMenu {
        id: menu
        objectName: "choiceMenu"
        look: chip.look
        parent: Overlay.overlay
        width: Math.max(chip.width, 24 * chip.look.unit)
        entries: chip.options.map(o => ({text: o.text, value: o.value, checked: o.value === chip.value}))
        onPicked: v => { chip.picked(v); chip.forceActiveFocus() }
    }
}
