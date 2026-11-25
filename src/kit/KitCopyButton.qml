import QtQuick
import QtQuick.Controls
import Fugue

// The copy button at the end of an error message (docs/ui-ergonomics.md, 2026-11-25 Decisions): a 6u round KitChip with the `copy` glyph.
// Pressing it (Enter and Space too) puts `text` on the clipboard as plain text, the whole message with its details and ids, what a
// person pastes into a bug report, and shows a check for `durCopied` ms. Nothing moves; the check is a swap, not an animation.
KitChip {
    id: button
    property string copyText // what goes on the clipboard
    property bool copied: false
    readonly property alias timer: reset
    round: true
    implicitHeight: 6 * look.unit
    glyph: copied ? "check" : "copy"
    tip: copied ? qsTr("Copied") : qsTr("Copy the message")
    ToolTip.visible: (hovered || copied) && tip !== "" && visible
    onTapped: {
        App.copyText(copyText)
        copied = true
        reset.restart()
    }
    Timer {
        id: reset
        interval: button.look.params.durCopied
        onTriggered: button.copied = false
    }
}
