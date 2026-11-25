import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"

// A several-line text in a well, for a prompt: it grows with its lines (3 to 8 shown, then it scrolls). Enter is a new line;
// Tab and Shift+Tab move on (a prompt is never a place to get stuck in), and Ctrl+Enter is not taken, so the window's Run
// reaches it. `text` in, `edited(text)` out.
Rectangle {
    id: area
    required property CanvasLook look
    property string text
    property string label // the screen reader's name
    signal edited(string text)

    readonly property real lineHeight: Math.round(look.textSize * 4 / 3)
    implicitHeight: Math.max(3, Math.min(8, edit.lineCount)) * lineHeight + 2 * look.unit + 8
    radius: look.smallCorner
    color: look.well
    border.width: edit.activeFocus ? 2 : 1
    border.color: edit.activeFocus ? look.accent : look.hairline

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
            width: flick.width - (flick.ScrollBar.vertical as FugueScrollBar).inset
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
            onTextChanged: if (text !== area.text) area.edited(text)
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
