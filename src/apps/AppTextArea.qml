import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "../kit"

// A wrapping text in a well: it grows with its lines (`minLines` to 8 shown, then it scrolls). Enter is a new line, unless
// `singleLine`: then Enter is left to the window and pasted line breaks become spaces (a one-line value that wraps to read).
// Tab and Shift+Tab move on (a prompt is never a place to get stuck in), and Ctrl+Enter is not taken, so the window's Run
// reaches it. `text` in, `edited(text)` out.
Rectangle {
    id: area
    required property CanvasLook look
    property string text
    property string label // the screen reader's name
    property int minLines: 3
    property bool singleLine: false
    signal edited(string text)

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
    TapHandler { onTapped: edit.forceActiveFocus() }
    readonly property alias editor: edit
}
