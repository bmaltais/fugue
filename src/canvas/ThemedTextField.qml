import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material.impl
import QtQuick.Templates as T
import Fugue
import "../kit"

// The one single-line text field of the window (docs/canvas-look.md 20.6): the Drift kit's recessed well (KitWell), 7u tall, the
// text 2.5u from each end, and a plain placeholder in `label` that stays where the text goes until the first letter is typed. It is
// a TextField of the Templates, not Material's: no 56px outlined box, no floating label, no accent underline. The selection is the
// theme's accent with its readable foreground (#74). Focused, the well's edge lifts to 1px of `label` at 45%; a field never draws
// the accent ring (owner 2026-11-25). `framed: false` leaves the well out, for a field that is the whole of its popup (a tab's rename).
T.TextField {
    id: control
    property bool framed: true

    implicitWidth: implicitBackgroundWidth + leftInset + rightInset
                   || Math.max(contentWidth, placeholder.implicitWidth) + leftPadding + rightPadding
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    padding: App.theme.look.unit
    leftPadding: 2.5 * App.theme.look.unit
    rightPadding: 2.5 * App.theme.look.unit
    verticalAlignment: TextInput.AlignVCenter
    color: enabled ? App.theme.look.text : App.theme.look.label
    placeholderTextColor: App.theme.look.label
    selectionColor: App.theme.look.accent
    selectedTextColor: App.theme.look.accentForeground
    opacity: enabled ? 1 : 0.6

    ContextMenu.menu: TextEditingContextMenu { editor: control } // cut, copy, paste on a right-click, as Material's field had

    Text {
        id: placeholder
        x: control.leftPadding
        y: control.topPadding
        width: control.width - (control.leftPadding + control.rightPadding)
        height: control.height - (control.topPadding + control.bottomPadding)
        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        verticalAlignment: control.verticalAlignment
        horizontalAlignment: control.effectiveHorizontalAlignment
        elide: Text.ElideRight
        renderType: control.renderType
        visible: control.length === 0 && control.preeditText === ""
    }

    background: Item {
        implicitWidth: 50 * App.theme.look.unit
        implicitHeight: 7 * App.theme.look.unit
        KitWell {
            visible: control.framed
            look: App.theme.look
            focused: control.activeFocus
        }
    }
}
