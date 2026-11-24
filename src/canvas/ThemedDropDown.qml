import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "widgets"

// A drop-down in Fugue's look: a well with a hairline edge (the accent edge while it has focus or is open), the
// current entry, a chevron, and the entries in the widgets' own list (WidgetMenu, as the model picker's). Not a
// Material ComboBox, which draws its own 56px underlined field. `options` are {value, text}; `value` is the current
// one; `picked(value)` says a choice. The current text and each entry are elided only when longer than `maxWidth`, the
// full current text is the tooltip; the list is as wide as its longest entry (at most the window less margins).
Rectangle {
    id: drop
    property var options: [] // {value, text, dim}
    property var value
    property real maxWidth: 48 * look.unit
    property string label // the screen reader's name
    // The list opens with a search field on top (word-start matching, WidgetMenu); the keys of the list stay.
    property bool searchable: false
    readonly property CanvasLook look: App.theme.look
    readonly property int currentIndex: Math.max(0, options.findIndex(o => o.value === value))
    readonly property string currentText: options.length > 0 ? options[currentIndex].text : ""
    signal picked(var value)

    implicitWidth: Math.min(maxWidth, textWidth.advanceWidth + 2 * look.unit + 4 * look.unit + chevron.width + look.unit)
    implicitHeight: 7 * look.unit
    // In a row that is too short it gives way down to this, eliding its text.
    Layout.minimumWidth: 12 * look.unit
    radius: look.smallCorner
    color: hover.hovered ? look.menuRow : look.well
    border.width: 1
    border.color: activeFocus || menu.opened ? look.accent : look.hairlineStrong
    activeFocusOnTab: true
    Accessible.role: Accessible.ComboBox
    Accessible.name: label
    Accessible.description: currentText
    Keys.onSpacePressed: open()
    Keys.onReturnPressed: open()
    Keys.onEnterPressed: open()
    Keys.onDownPressed: open()

    function open() {
        const at = drop.mapToItem(Overlay.overlay, 0, drop.height)
        menu.wantX = at.x
        menu.wantY = at.y
        menu.anchorHeight = drop.height
        menu.open()
    }

    TextMetrics { id: textWidth; font.pixelSize: drop.look.textSize; text: drop.currentText }
    TextMetrics { id: longest; font.pixelSize: drop.look.labelSize
        text: drop.options.reduce((a, o) => o.text.length > a.length ? o.text : a, "") }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: { drop.forceActiveFocus(); drop.open() } }
    ToolTip.visible: hover.hovered && !menu.opened && textWidth.advanceWidth > text.width
    ToolTip.delay: look.tooltipDelay
    ToolTip.text: currentText

    Text {
        id: text
        anchors { left: parent.left; leftMargin: 2 * drop.look.unit; right: chevron.left; rightMargin: drop.look.unit
                  verticalCenter: parent.verticalCenter }
        text: drop.currentText
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: drop.look.text
        font.pixelSize: drop.look.textSize
    }
    Glyph {
        id: chevron
        anchors { right: parent.right; rightMargin: drop.look.unit; verticalCenter: parent.verticalCenter }
        look: drop.look
        name: "chevron-down"
        color: drop.look.label
        font.pixelSize: 12
    }

    WidgetMenu {
        id: menu
        look: drop.look
        parent: Overlay.overlay
        searchable: drop.searchable
        wordStart: true
        nothingMatches: qsTr("Nothing matches “%1”")
        // Wide enough to read the longest entry whole (the check, the text and the row's padding), never wider than the window.
        width: Math.min(Math.max(drop.width, longest.advanceWidth + 8 + 12 + 6 + 8 + 2 + 12), Overlay.overlay ? Overlay.overlay.width - 4 * drop.look.unit : 400)
        entries: drop.options.map(o => ({ text: o.text, value: o.value, checked: o.value === drop.value, dim: o.dim === true }))
        onPicked: v => { drop.picked(v); drop.forceActiveFocus() }
    }
}
