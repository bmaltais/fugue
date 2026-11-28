import QtQuick
import QtQuick.Controls
import Fugue
import ".."

// A multiline widget's field while it is typed into: it opens over the rows below with room
// for several lines, so a prompt can be read whole. Enter is a new line; leaving the field,
// Escape or Tab ends the edit and keeps the text (a long prompt is never thrown away by a
// key), as one undo step. Each change is in the document as it is typed.
Rectangle {
    id: area
    required property InputWidget host
    readonly property real rowHeight: host.height
    width: host.width
    z: 2
    property bool closing: false
    property bool ready: false
    // Where the press was in the well (host coordinates), or -1: the caret goes there; with none (Tab into the field) it goes to the end.
    property real caretX: -1
    property real caretY: -1
    // The text sits exactly where the resting well's does (InputWidget's `widgetValue`: its font, a fixed 16 px line, the same top), so a
    // click on a line of the resting text puts the caret on that line of the editor, and nothing moves when the editor opens.
    readonly property real lineHeight: 16
    readonly property real textTop: Math.round(1.5 * host.look.unit) + 16 // under the caption line
    // The text as last seen: the highlighter's formatting passes also report a change of the text, with the text as it was.
    property string lastText: ""

    height: Math.max(rowHeight, Math.min(Math.max(5, edit.lineCount + 1), 12) * lineHeight + textTop + 4)
    color: host.wellColor
    radius: host.smallCorner
    border.width: 1
    border.color: host.error !== "" ? host.look.error : host.look.accent

    // The field's label stays as its caption while it is typed in (#74).
    Text {
        x: area.host.pad
        y: 4
        height: 16
        verticalAlignment: Text.AlignVCenter
        text: area.host.port.label
        color: area.host.look.label
        font.pixelSize: area.host.look.labelSize
    }
    // The input's manifest `syntax` colours the text as in the text window (its problems underlined; the window's footer names them).
    PromptHighlight {
        objectName: "widgetHighlight"
        target: edit.textDocument
        syntax: area.host.edits.spec(area.host.nodeId, area.host.name).syntax || ""
        accent: area.host.look.accent
        label: area.host.look.label
        role: area.host.look.role("blue").frameEdge
        error: area.host.look.error
        well: area.host.wellColor
    }
    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 4
        anchors.topMargin: area.textTop
        anchors.leftMargin: area.host.pad
        anchors.rightMargin: area.host.pad
        contentWidth: width
        contentHeight: edit.contentHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height
        ScrollBar.vertical: FugueScrollBar { look: area.host.look }

        TextEdit {
            id: edit
            objectName: "widgetArea"
            // The room the scroll bar takes: set when the text overflows, not bound (the bar shows by the text's height, which the width
            // decides; and the field's own height follows the line count: a binding loop).
            property real gutter: 0
            width: flick.width - gutter
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            textFormat: TextEdit.PlainText
            color: area.host.look.text
            selectionColor: area.host.look.accent
            selectedTextColor: area.host.look.accentForeground
            font.pixelSize: area.host.labelPx
            // Only a change of the text is typing: the highlighter's passes (and a closing editor) must not preview, or old text could go
            // into the next field's session when a key and Tab arrive in one batch.
            onTextChanged: {
                if (!area.ready || area.closing || text === area.lastText)
                    return
                area.lastText = text
                area.host.error = area.host.edits.preview(text)
            }
            onContentHeightChanged: gutter = contentHeight > flick.height ? (flick.ScrollBar.vertical as FugueScrollBar).breadth : 0
            onCursorRectangleChanged: {
                if (cursorRectangle.y < flick.contentY)
                    flick.contentY = cursorRectangle.y
                else if (cursorRectangle.y + cursorRectangle.height > flick.contentY + flick.height)
                    flick.contentY = cursorRectangle.y + cursorRectangle.height - flick.height
            }
            // Escape is the field's, before any shortcut on it (run.cancel while a run is going).
            Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape
            Keys.onEscapePressed: area.done(true)
            Keys.onTabPressed: event => area.next(event.modifiers & Qt.ShiftModifier ? -1 : 1)
            Keys.onBacktabPressed: area.next(-1)
            onActiveFocusChanged: if (!activeFocus) area.done(false)
        }
    }

    // Zoomed out past reading, or the node collapsed: the edit ends and keeps what is valid.
    onVisibleChanged: if (!visible) area.done(true)

    // The row can go while the field is open (the node collapsed): the edit still ends.
    property var sessionEdits: null
    Component.onDestruction: if (!closing && sessionEdits) sessionEdits.finish()

    Component.onCompleted: {
        sessionEdits = host.edits
        host.edits.begin(host.nodeId, host.name)
        edit.text = host.startText()
        lastText = edit.text
        TextLines.setFixedLineHeight(edit.textDocument, lineHeight)
        edit.cursorPosition = caretX < 0 ? edit.length
                              : edit.positionAt(caretX - flick.anchors.leftMargin, caretY - flick.anchors.topMargin + flick.contentY)
        ready = true
        edit.forceActiveFocus()
    }

    function next(step) {
        done(false)
        if (!host.focusNeighbour(step))
            host.giveFocusBack()
    }
    function done(refocus) {
        if (closing)
            return
        closing = true
        host.edits.finish()
        host.closeEditor()
        if (refocus)
            host.giveFocusBack()
    }

    Connections {
        target: area.host.edits
        function onSessionEnded() {
            if (area.closing)
                return
            area.closing = true
            area.host.closeEditor()
            area.host.giveFocusBack()
        }
    }

    ErrorBubble {
        host: area.host
        y: area.height + 2
        width: area.width
    }
}
