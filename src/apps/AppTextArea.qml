import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "../kit"

// A wrapping text in a well: it grows with its lines (`minLines` to 8 shown, then it scrolls). Enter is a new line, unless
// `singleLine`: then Enter is left to the window and pasted line breaks become spaces (a one-line value that wraps to read).
// Tab and Shift+Tab move on (a prompt is never a place to get stuck in), and Ctrl+Enter is not taken, so the window's Run
// reaches it. `text` in, `edited(text)` out.
// `expandable` (#715 slice 5): while the pointer is over the well an expand button (Lucide `maximize-2`) shows at its top right, and
// Alt+T in the field does the same: both ask for the text window (`expandRequested`), a larger editor on this field's own value.
Rectangle {
    id: area
    required property CanvasLook look
    property string text
    property string label // the screen reader's name
    property int minLines: 3
    property bool singleLine: false
    property bool expandable: false
    signal edited(string text)
    signal expandRequested()

    readonly property real lineHeight: Math.round(look.textSize * 4 / 3)
    implicitHeight: Math.max(minLines, Math.min(8, edit.lineCount)) * lineHeight + 2 * look.unit + 8
    radius: look.params.cornerWell * look.unit
    color: "transparent"
    KitWell { look: area.look; focused: edit.activeFocus }

    Flickable {
        id: flick
        anchors { fill: parent; margins: area.look.unit + 2 }
        contentWidth: width
        contentHeight: edit.contentHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height
        ScrollBar.vertical: FugueScrollBar { look: area.look }
        TextEdit {
            id: edit
            objectName: area.objectName + "-edit"
            // The bar's breadth always, not its `inset` (zero while it hides): the text wrapping to its lines showed the bar, whose inset narrowed
            // the text, which wrapped to more lines: a binding loop.
            width: flick.width - (flick.ScrollBar.vertical as FugueScrollBar).width
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            textFormat: TextEdit.PlainText
            color: area.look.text
            selectionColor: area.look.accent
            selectedTextColor: area.look.accentForeground
            font.pixelSize: area.look.textSize
            text: area.text
            activeFocusOnTab: true
            Accessible.role: Accessible.EditableText
            Accessible.name: area.label
            onTextChanged: {
                // A paste into a one-line value: its line breaks are spaces.
                // Sent out, not assigned: the value comes back through `text: area.text`, and assigning `text` here would cut
                // that binding, so a value set later (another tab, a reset) would no longer show.
                if (area.singleLine && /[\r\n]/.test(text)) {
                    const at = cursorPosition
                    area.edited(text.replace(/\r\n|\r|\n/g, " "))
                    cursorPosition = Math.min(at, length)
                    return
                }
                if (text !== area.text) area.edited(text)
            }
            // Alt+T is the text window's key (the canvas's Edit text…): the field takes it before the window's shortcuts.
            Keys.onShortcutOverride: event => { if (area.expandable && event.key === Qt.Key_T && event.modifiers === Qt.AltModifier) event.accepted = true }
            Keys.onPressed: event => {
                if (area.expandable && event.key === Qt.Key_T && event.modifiers === Qt.AltModifier) {
                    area.expandRequested()
                    event.accepted = true
                }
            }
            Keys.onReturnPressed: event => { event.accepted = area.singleLine && !(event.modifiers & Qt.ControlModifier) }
            Keys.onEnterPressed: event => { event.accepted = area.singleLine && !(event.modifiers & Qt.ControlModifier) }
            Keys.onTabPressed: event => { nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocusReason); event.accepted = true }
            Keys.onBacktabPressed: event => { nextItemInFocusChain(false).forceActiveFocus(Qt.BacktabFocusReason); event.accepted = true }
            onCursorRectangleChanged: {
                if (cursorRectangle.y < flick.contentY)
                    flick.contentY = cursorRectangle.y
                else if (cursorRectangle.y + cursorRectangle.height > flick.contentY + flick.height)
                    flick.contentY = cursorRectangle.y + cursorRectangle.height - flick.height
            }
        }
    }
    TapHandler { gesturePolicy: TapHandler.WithinBounds; onTapped: edit.forceActiveFocus() }
    HoverHandler { id: over }
    // The button has the well's fill behind it: the text runs under it.
    Rectangle {
        objectName: area.objectName + "-expand"
        visible: area.expandable && (over.hovered || expandButton.hovered)
        anchors { top: parent.top; right: parent.right; topMargin: 2; rightMargin: 2 }
        width: 24
        height: 24
        radius: area.look.smallCorner
        color: Qt.tint(area.look.kitGroup, Qt.rgba(0, 0, 0, area.look.light ? area.look.params.wellDarkenLight : area.look.params.wellDarken))
        ViewButton {
            id: expandButton
            anchors.fill: parent
            look: area.look
            compact: true
            flat: true
            glyph: "maximize-2"
            tip: qsTr("Edit in a larger window (Alt+T)")
            onTapped: area.expandRequested()
        }
    }
    readonly property alias editor: edit
}
