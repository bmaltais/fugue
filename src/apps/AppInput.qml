pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue
import "../canvas"

// One input of an app's form (#385): its label over its control, the control chosen by the field's kind. This slice has the
// plain ones (text, number, choice; a file shows its path as text); #386 swaps the control per kind (image drop, mask paint,
// seed modes, model picker) in this one place. It knows nothing of the run or the workflow: give it a `field` (AppView's:
// {key, label, kind, value, min, max, choices}) and listen to `edited(key, value)`; a number cleared says "".
ColumnLayout {
    id: root
    required property var field
    readonly property CanvasLook look: App.theme.look
    // What the control shows now: starts as the field's value, follows what the person does.
    property var current: field.value
    signal edited(string key, var value)

    spacing: look.unit
    Accessible.role: Accessible.Grouping
    Accessible.name: field.label

    Text {
        Layout.fillWidth: true
        text: root.field.label
        color: root.look.label
        font.pixelSize: root.look.labelSize
        elide: Text.ElideRight
    }
    Loader {
        Layout.fillWidth: true
        sourceComponent: root.field.kind === "choice" ? choiceInput
                       : root.field.kind === "number" && root.field.min !== undefined && root.field.max !== undefined ? rangedInput
                       : root.field.kind === "number" ? numberInput : textInput
    }
    Component {
        id: textInput
        ThemedTextField {
            objectName: "appInput-" + root.field.key
            text: root.current === undefined || root.current === null ? "" : String(root.current)
            font.pixelSize: root.look.textSize
            selectByMouse: true
            Accessible.name: root.field.label
            onTextEdited: {
                root.current = text
                root.edited(root.field.key, text)
            }
        }
    }
    Component {
        id: numberInput
        ThemedTextField {
            objectName: "appInput-" + root.field.key
            text: root.current === undefined || root.current === null ? "" : String(root.current)
            placeholderText: qsTr("Automatic")
            font.pixelSize: root.look.textSize
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
            selectByMouse: true
            Accessible.name: root.field.label
            // Each edit is a value (not the end of editing): a Run pressed with the cursor still here uses what is shown.
            onTextEdited: {
                root.current = text
                root.edited(root.field.key, text)
            }
        }
    }
    Component {
        id: rangedInput
        DragNumberField {
            objectName: "appInput-" + root.field.key
            look: root.look
            width: parent?.width ?? 0
            from: root.field.min
            to: root.field.max
            step: Number.isInteger(root.field.min) && Number.isInteger(root.field.max) ? 1 : 0.01
            value: Number(root.current ?? root.field.min)
            defaultValue: Number(root.field.value ?? root.field.min)
            onCommitted: v => {
                root.current = v
                root.edited(root.field.key, v)
            }
        }
    }
    Component {
        id: choiceInput
        ThemedDropDown {
            objectName: "appInput-" + root.field.key
            options: root.field.choices
            value: root.current
            label: root.field.label
            maxWidth: 1000 * root.look.unit
            onPicked: v => {
                root.current = v
                root.edited(root.field.key, v)
            }
        }
    }
}
