import QtQuick
import Fugue

// A title edited in place (#74), on a node's header or a Frame: it opens with the title selected;
// Enter keeps it, Esc drops it, and leaving the field (a click elsewhere) keeps it. `done` says
// which, once.
TextInput {
    id: editor
    objectName: "titleEditor"
    required property string initial
    signal done(bool keep)
    property bool finished: false

    function finish(keep) {
        if (finished)
            return
        finished = true
        done(keep)
    }

    text: initial
    clip: true
    selectByMouse: true
    selectionColor: App.theme.look.accent
    selectedTextColor: App.theme.look.accentForeground
    Component.onCompleted: {
        forceActiveFocus()
        selectAll()
    }
    Keys.onReturnPressed: finish(true)
    Keys.onEnterPressed: finish(true)
    // Esc is this field's while it is typed in, not the window's (Leave group, Cancel run).
    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape
    Keys.onEscapePressed: finish(false)
    onActiveFocusChanged: if (!activeFocus) finish(true)
}
