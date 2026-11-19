import QtQuick

// A widget's one-line field while it is typed into (spin, slider, seed, line, file, folder,
// color, a model's name). Each valid text is in the document as it is typed, and the whole
// edit is one undo step (WidgetEdit's session): Enter or leaving the field ends it, Escape
// puts the value back, Tab and Shift+Tab end it and go to the node's next or previous field.
// Up and Down step a number. An invalid text shows why and is not written.
Rectangle {
    id: field
    required property Item host
    property bool closing: false
    readonly property bool numeric: ["spin", "slider", "seed"].indexOf(host.kind) >= 0

    width: host.width
    height: host.height
    z: 2

    color: host.wellColor
    radius: host.smallCorner
    border.width: 1
    border.color: host.error !== "" ? host.look.error : host.look.accent

    TextInput {
        id: input
        objectName: "widgetInput"
        anchors.fill: parent
        anchors.leftMargin: field.host.pad
        anchors.rightMargin: field.host.pad
        verticalAlignment: TextInput.AlignVCenter
        horizontalAlignment: field.numeric ? TextInput.AlignRight : TextInput.AlignLeft
        clip: true
        selectByMouse: true
        color: field.host.look.text
        selectionColor: field.host.look.accent
        selectedTextColor: theme.accentForeground
        font.pixelSize: field.host.look.smallText
        font.weight: Font.Medium

        onTextEdited: field.host.error = field.host.edits.preview(text)
        Keys.onReturnPressed: field.enter()
        Keys.onEnterPressed: field.enter()
        Keys.onEscapePressed: field.done(false, true)
        Keys.onTabPressed: event => field.next(event.modifiers & Qt.ShiftModifier ? -1 : 1)
        Keys.onBacktabPressed: field.next(-1)
        Keys.onUpPressed: event => field.step(1, event)
        Keys.onDownPressed: event => field.step(-1, event)
        onActiveFocusChanged: if (!activeFocus) field.done(true, false)
    }

    Component.onCompleted: {
        host.edits.begin(host.nodeId, host.name)
        input.text = host.startText()
        input.selectAll()
        input.forceActiveFocus()
    }

    function enter() {
        if (host.error === "")
            done(true, true)
    }
    function next(step) {
        done(true, false)
        if (!host.focusNeighbour(step))
            host.giveFocusBack()
    }
    function step(steps, event) {
        if (!numeric) {
            event.accepted = false
            return
        }
        const typed = Number(input.text.trim().replace(",", "."))
        const from = input.text.trim() !== "" && !isNaN(typed) ? typed : host.number
        input.text = host.edits.numberText(host.edits.stepped(host.nodeId, host.name, from, steps))
        host.error = host.edits.preview(input.text)
    }
    function done(keep, refocus) {
        if (closing)
            return
        closing = true
        if (keep)
            host.edits.finish()
        else
            host.edits.cancel()
        host.closeEditor()
        if (refocus)
            host.giveFocusBack()
    }

    Connections {
        target: field.host.edits
        function onSessionDropped() {
            if (field.closing)
                return
            field.closing = true
            field.host.closeEditor()
        }
    }

    ErrorBubble {
        host: field.host
        y: field.height + 2
        width: field.width
    }
}
