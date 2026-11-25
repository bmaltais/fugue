import QtQuick
import QtQuick.Layouts
import Fugue
import "../canvas"

// A seed as a node has it: the number, a dice for a new one, and its mode: Fixed (the run uses the number), Randomize each
// run (a new one every time) or Increment after each run (the number, then the next). The mode and the number live in
// AppView (setSeedMode, rollSeed, setValue); this only shows them.
RowLayout {
    id: row
    required property var field
    required property AppView appView
    required property CanvasLook look
    property string mode: field.mode
    property var current: field.value
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
    ThemedTextField {
        objectName: "appInput-" + row.field.key
        Layout.fillWidth: true
        enabled: row.mode !== "random"
        text: row.mode === "random" || row.current === undefined || row.current === null ? "" : String(row.current)
        placeholderText: row.mode === "random" ? qsTr("New each run") : qsTr("Automatic")
        font.pixelSize: row.look.textSize
        inputMethodHints: Qt.ImhDigitsOnly
        validator: RegularExpressionValidator { regularExpression: /^[0-9]{0,16}$/ }
        selectByMouse: true
        Accessible.name: row.field.label
        onTextEdited: {
            row.current = text
            row.appView.setValue(row.field.key, text)
        }
    }
    ViewButton {
        objectName: "appSeedRoll-" + row.field.key
        look: row.look
        glyph: "dices"
        tip: qsTr("A new random number")
        enabled: row.mode !== "random"
        onTapped: row.appView.rollSeed(row.field.key)
    }
    ThemedDropDown {
        objectName: "appSeedMode-" + row.field.key
        options: [{ value: "fixed", text: qsTr("Fixed") },
                  { value: "random", text: qsTr("Randomize each run") },
                  { value: "increment", text: qsTr("Increment after each run") }]
        value: row.mode
        label: qsTr("%1 mode").arg(row.field.label)
        onPicked: v => row.appView.setSeedMode(row.field.key, v)
    }
}
