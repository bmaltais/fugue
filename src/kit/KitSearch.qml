pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../canvas"

// A search field of the Drift kit (docs/canvas-look.md 20.19): a recessed well (KitWell) with a search glyph, the placeholder and
// a clear button while there is text. Value in the `text`, which the person edits (the host reads it live); `accepted` is Enter
// and `down` the Down arrow, which a host uses to go to the list below. Escape is left to the dialog, which closes (the clear
// button empties the field). Tab reaches it, and the 2px accent ring shows while it has the keyboard. `focusField()` gives it the
// keyboard.
FocusScope {
    id: field
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property string placeholder
    property alias text: input.text
    signal accepted
    signal down

    function focusField() { input.forceActiveFocus() }
    readonly property bool hasFocus: input.activeFocus

    implicitHeight: 7 * look.unit
    implicitWidth: 40 * look.unit
    KitWell { look: field.look; focused: input.activeFocus }
    Glyph {
        id: lens
        x: 2.5 * field.look.unit
        anchors.verticalCenter: parent.verticalCenter
        look: field.look
        name: "search"
        color: field.look.label
    }
    TextInput {
        id: input
        objectName: field.objectName !== "" ? field.objectName + "Input" : ""
        anchors { left: lens.right; leftMargin: 1.5 * field.look.unit; right: clear.left; rightMargin: field.look.unit
                  verticalCenter: parent.verticalCenter }
        focus: true
        clip: true
        selectByMouse: true
        color: field.look.text
        selectionColor: field.look.accent
        selectedTextColor: field.look.accentForeground
        font.pixelSize: field.look.textSize
        Accessible.role: Accessible.EditableText
        Accessible.name: field.placeholder
        Text {
            visible: input.text === "" && !input.inputMethodComposing
            anchors.verticalCenter: parent.verticalCenter
            text: field.placeholder
            color: field.look.label
            font: input.font
        }
        Keys.onDownPressed: field.down()
        Keys.onReturnPressed: field.accepted()
        Keys.onEnterPressed: field.accepted()
    }
    Item { // the clear button: a press target of 7u, the glyph inside
        id: clear
        objectName: field.objectName !== "" ? field.objectName + "Clear" : ""
        visible: input.text !== ""
        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
        width: visible ? 7 * field.look.unit : 0
        height: width
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Clear the search")
        Glyph { anchors.centerIn: parent; look: field.look; name: "x"; color: clearHover.hovered ? field.look.text : field.look.label }
        HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { gesturePolicy: TapHandler.WithinBounds; onTapped: { input.text = ""; input.forceActiveFocus() } }
    }
}
