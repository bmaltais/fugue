import QtQuick

// A multiline widget's field while it is typed into: it opens over the rows below with room
// for several lines, so a prompt can be read whole. Enter is a new line; leaving the field,
// Escape or Tab ends the edit and keeps the text (a long prompt is never thrown away by a
// key), as one undo step. Each change is in the document as it is typed.
Rectangle {
    id: area
    required property Item host
    readonly property real rowHeight: host.height
    width: host.width
    z: 2
    property bool closing: false
    property bool ready: false
    readonly property real lineHeight: Math.round(host.look.textSize * 4 / 3)

    height: Math.max(rowHeight, Math.min(Math.max(5, edit.lineCount + 1), 12) * lineHeight + 8)
    color: host.wellColor
    radius: host.smallCorner
    border.width: 1
    border.color: host.error !== "" ? host.look.error : host.look.accent

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 4
        anchors.leftMargin: area.host.pad
        anchors.rightMargin: area.host.pad
        contentWidth: width
        contentHeight: edit.contentHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height

        TextEdit {
            id: edit
            objectName: "widgetArea"
            width: flick.width
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            textFormat: TextEdit.PlainText
            color: area.host.look.text
            selectionColor: area.host.look.accent
            selectedTextColor: theme.accentForeground
            font.pixelSize: area.host.look.smallText
            onTextChanged: if (area.ready) area.host.error = area.host.edits.preview(text)
            onCursorRectangleChanged: {
                if (cursorRectangle.y < flick.contentY)
                    flick.contentY = cursorRectangle.y
                else if (cursorRectangle.y + cursorRectangle.height > flick.contentY + flick.height)
                    flick.contentY = cursorRectangle.y + cursorRectangle.height - flick.height
            }
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
        edit.selectAll()
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
