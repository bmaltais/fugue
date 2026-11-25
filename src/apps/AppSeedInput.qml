import QtQuick
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../kit"

// A seed as a node has it, in one row of the height of the other fields (7u): the number in a plain well (no Material outline,
// no floating label: the mode is said by the mode control alone), a dice KitChip pill for a new number, and the mode as a chip
// menu (AppChoice): Fixed (the run uses the number), Random (a new one every run, the field then shows "New each run") or Increment
// (the number, then the next after each run). The mode and the number live in AppView (setSeedMode, rollSeed, setValue);
// this only shows them.
RowLayout {
    id: row
    required property var field
    required property AppView appView
    required property CanvasLook look
    property string mode: field.mode
    property var current: field.value
    readonly property bool random: mode === "random"
    spacing: look.unit

    Connections {
        target: row.appView
        function onValueChanged(key) {
            if (key !== row.field.key)
                return
            row.mode = row.appView.seedMode(key)
            row.current = row.appView.valueOf(key)
        }
    }
    Rectangle {
        id: well
        Layout.fillWidth: true
        Layout.minimumWidth: 16 * row.look.unit
        implicitHeight: 7 * row.look.unit
        radius: row.look.smallCorner
        color: "transparent"
        KitWell { look: row.look; focused: input.activeFocus } // recessed, as every field
        opacity: row.random ? 0.6 : 1
        TextInput {
            id: input
            objectName: "appInput-" + row.field.key
            anchors { fill: parent; leftMargin: 2 * row.look.unit; rightMargin: 2 * row.look.unit }
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            enabled: !row.random
            text: row.random || row.current === undefined || row.current === null ? "" : String(row.current)
            color: row.look.text
            selectionColor: row.look.accent
            selectedTextColor: row.look.accentForeground
            font.pixelSize: row.look.textSize
            font.features: { "tnum": 1 }
            inputMethodHints: Qt.ImhDigitsOnly
            validator: RegularExpressionValidator { regularExpression: /^[0-9]{0,16}$/ }
            selectByMouse: true
            activeFocusOnTab: true
            Accessible.role: Accessible.EditableText
            Accessible.name: row.field.label
            onTextEdited: {
                row.current = text
                row.appView.setValue(row.field.key, text)
            }
        }
        Text {
            anchors { fill: parent; leftMargin: 2 * row.look.unit; rightMargin: 2 * row.look.unit }
            verticalAlignment: Text.AlignVCenter
            visible: row.random
            text: qsTr("New each run")
            color: row.look.label
            font.pixelSize: row.look.textSize
            elide: Text.ElideRight
        }
    }
    KitChip {
        objectName: "appSeedRoll-" + row.field.key
        look: row.look
        pill: true
        glyph: "dices"
        tip: qsTr("A new random number")
        enabled: !row.random
        onTapped: row.appView.rollSeed(row.field.key)
    }
    AppChoice {
        objectName: "appSeedMode-" + row.field.key
        look: row.look
        Layout.preferredWidth: 20 * row.look.unit
        options: [{ value: "fixed", text: qsTr("Fixed") },
                  { value: "random", text: qsTr("Random") },
                  { value: "increment", text: qsTr("Increment") }]
        value: row.mode
        label: qsTr("%1 mode").arg(row.field.label)
        onPicked: v => row.appView.setSeedMode(row.field.key, v)
    }
}
