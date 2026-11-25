import QtQuick
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../kit"

// The App form's one text field: a well 7u tall with a hairline edge (the accent edge while it has the focus), the text in the
// body size and a dim placeholder. Not Material's tall outlined field. `textEdited` says each edit (not the end of editing), as
// a Run pressed with the cursor still in it must use what is shown; `accepted` is Enter. Tab reaches it.
Rectangle {
    id: field
    property CanvasLook look: App.theme.look
    property alias text: input.text
    property string placeholder
    property alias validator: input.validator
    property alias inputMethodHints: input.inputMethodHints
    property alias horizontalAlignment: input.horizontalAlignment
    property string label // the screen reader's name
    readonly property alias editing: input.activeFocus
    signal textEdited
    signal accepted
    signal editingFinished

    implicitHeight: 7 * look.unit
    implicitWidth: 16 * look.unit
    radius: look.smallCorner
    color: "transparent"
    KitWell { look: field.look; focused: input.activeFocus }
    opacity: enabled ? 1 : 0.5
    Accessible.role: Accessible.EditableText
    Accessible.name: label
    function forceFocus() { input.forceActiveFocus() }

    TextInput {
        id: input
        anchors { fill: parent; leftMargin: 2 * field.look.unit; rightMargin: 2 * field.look.unit }
        verticalAlignment: TextInput.AlignVCenter
        color: field.look.text
        font.pixelSize: field.look.textSize
        selectByMouse: true
        clip: true
        selectionColor: field.look.accent
        selectedTextColor: field.look.accentForeground
        activeFocusOnTab: true
        onTextEdited: field.textEdited()
        onAccepted: field.accepted()
        onEditingFinished: field.editingFinished()
        Text {
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            visible: input.text === "" && !input.activeFocus
            text: field.placeholder
            color: field.look.label
            font.pixelSize: field.look.textSize
            elide: Text.ElideRight
        }
    }
}
