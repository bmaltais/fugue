pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue
import "../canvas"

// One input of an app's form (#385, #386): its label over its control, the control chosen by the field's kind, in this one
// place: text (a well of several lines for a prompt), number (a drag field with a range, else typed), toggle, choice, seed
// (number, dice and mode), image (drop, paste, pick, samples, last used), mask (the mask editor over the picture), model
// (the model menu with Download). It knows nothing of the run or the workflow: give it a `field` (AppView's: {key, label,
// kind, value, min, max, choices, ...}) and listen to `edited(key, value)`; a number cleared says "". The kinds that take
// files, seeds and models act on the store (`appView`) themselves. Every control is reached with Tab and worked with
// Enter or Space; Ctrl+Enter, Run, is the window's and no field takes it.
ColumnLayout {
    id: root
    required property var field
    readonly property CanvasLook look: App.theme.look
    property AppView appView: App.appView
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
                       : root.field.kind === "number" ? numberInput
                       : root.field.kind === "toggle" ? toggleInput
                       : root.field.kind === "seed" ? seedInput
                       : root.field.kind === "image" ? imageInput
                       : root.field.kind === "mask" ? maskInput
                       : root.field.kind === "model" ? modelInput
                       : root.field.multiline === true ? areaInput : textInput
    }
    Component {
        id: areaInput
        AppTextArea {
            objectName: "appInput-" + root.field.key
            width: parent?.width ?? 0
            look: root.look
            label: root.field.label
            text: root.current === undefined || root.current === null ? "" : String(root.current)
            onEdited: t => {
                root.current = t
                root.edited(root.field.key, t)
            }
        }
    }
    Component {
        id: toggleInput
        AppToggle {
            objectName: "appInput-" + root.field.key
            look: root.look
            label: root.field.label
            checked: root.current === true
            onToggled: on => {
                root.current = on
                root.edited(root.field.key, on)
            }
        }
    }
    Component {
        id: seedInput
        AppSeedInput {
            width: parent?.width ?? 0
            field: root.field
            appView: root.appView
            look: root.look
        }
    }
    Component {
        id: imageInput
        AppImageInput {
            width: parent?.width ?? 0
            field: root.field
            appView: root.appView
            look: root.look
        }
    }
    Component {
        id: maskInput
        AppMaskInput {
            width: parent?.width ?? 0
            field: root.field
            appView: root.appView
            look: root.look
        }
    }
    Component {
        id: modelInput
        AppModelInput {
            width: parent?.width ?? 0
            field: root.field
            appView: root.appView
            look: root.look
        }
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
