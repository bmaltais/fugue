import QtQuick
import QtQuick.Controls
import "../canvas/widgets"
import "../kit"

// A choice of the app panel as a KitDropdown (raised: it is clicked; owner, 2026-11-25): the chosen word and a chevron, opening the window's
// menu of the options (arrows move, Enter picks, Escape closes). `options` are {value, text}; `value` is the chosen one;
// `picked(value)` says the person chose, it never sets `value` itself.
KitDropdown {
    id: chip
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
    text: shownText
    elide: Text.ElideRight
    open: menu.opened
    Accessible.name: label
    onTapped: chip.show()
    function show() {
        const at = chip.mapToItem(Overlay.overlay, 0, chip.height)
        menu.wantX = at.x
        menu.wantY = at.y
        menu.anchorHeight = chip.height
        menu.open()
    }
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
