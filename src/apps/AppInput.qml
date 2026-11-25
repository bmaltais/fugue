pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../kit"

// One input of an app's form (#385, #386) as a row of the panel's KitGroup (docs/canvas-look.md "Drift kit"): a switch is a
// KitRow with the KitToggle at its end (a click anywhere on the row flips it), a choice a KitRow with a chip menu at its end,
// and everything wider (text and prompt, number, a range as KitSlider with its value, seed, picture, mask, model) an AppRow, the
// label and a dim hint over the control at full width. The control is chosen by the field's kind in this one place. It knows
// nothing of the run or the workflow: give it a `field` (AppView's: {key, label, kind, value, min, max, choices, hint?, ...})
// and listen to `edited(key, value)`; a number cleared says "". The kinds that take files, seeds and models act on the store
// (`appView`) themselves. Every control is reached with Tab and worked with Enter or Space; Ctrl+Enter, Run, is the window's
// and no field takes it. It draws the group's hairline above itself unless it is the first row.
Item {
    id: root
    required property var field
    readonly property CanvasLook look: App.theme.look
    property AppView appView: App.appView
    // What the control shows now: starts as the field's value, follows what the person does.
    property var current: field.value
    readonly property string hint: field.hint ?? ""
    signal edited(string key, var value)

    width: parent ? parent.width : implicitWidth
    implicitHeight: rows.item ? (rows.item as Item).height : 0
    height: implicitHeight
    Accessible.role: Accessible.Grouping
    Accessible.name: field.label

    // A value the store changes by itself (Reuse inputs, a sample) is what the control shows.
    Connections {
        target: root.appView
        function onValueChanged(key) {
            if (key === root.field.key && root.field.kind !== "seed" && root.field.kind !== "image")
                root.current = root.appView.valueOf(key)
        }
    }
    KitSeparator {
        look: root.look
        visible: root.y > 0
        z: 1
    }
    Loader {
        id: rows
        width: parent.width
        sourceComponent: root.field.kind === "toggle" ? toggleRow : root.field.kind === "choice" ? choiceRow : wideRow
    }
    Component {
        id: toggleRow
        KitRow {
            look: root.look
            label: root.field.label
            hint: root.hint
            target: sw
            KitToggle {
                id: sw
                objectName: "appInput-" + root.field.key
                look: root.look
                text: root.field.label
                checked: root.current === true
                onToggled: on => {
                    root.current = on
                    root.edited(root.field.key, on)
                }
            }
        }
    }
    Component {
        id: choiceRow
        KitRow {
            look: root.look
            label: root.field.label
            hint: root.hint
            AppChoice {
                objectName: "appInput-" + root.field.key
                look: root.look
                options: root.field.choices
                value: root.current
                label: root.field.label
                onPicked: v => {
                    root.current = v
                    root.edited(root.field.key, v)
                }
            }
        }
    }
    Component {
        id: wideRow
        AppRow {
            look: root.look
            label: root.field.label
            hint: root.hint
            Loader {
                width: parent.width
                sourceComponent: root.field.kind === "number" && root.field.min !== undefined && root.field.max !== undefined ? rangedInput
                               : root.field.kind === "number" ? numberInput
                               : root.field.kind === "seed" ? seedInput
                               : root.field.kind === "image" ? imageInput
                               : root.field.kind === "mask" ? maskInput
                               : root.field.kind === "model" ? modelInput
                               : root.field.multiline === true ? areaInput : textInput
            }
        }
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
        AppField {
            objectName: "appInput-" + root.field.key
            look: root.look
            width: parent?.width ?? 0
            text: root.current === undefined || root.current === null ? "" : String(root.current)
            label: root.field.label
            onTextEdited: {
                root.current = text
                root.edited(root.field.key, text)
            }
        }
    }
    Component {
        id: numberInput
        AppField {
            objectName: "appInput-" + root.field.key
            look: root.look
            width: parent?.width ?? 0
            text: root.current === undefined || root.current === null ? "" : String(root.current)
            placeholder: qsTr("Automatic")
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
            label: root.field.label
            // Each edit is a value (not the end of editing): a Run pressed with the cursor still here uses what is shown.
            onTextEdited: {
                root.current = text
                root.edited(root.field.key, text)
            }
        }
    }
    Component {
        id: rangedInput
        KitSlider {
            objectName: "appInput-" + root.field.key
            look: root.look
            width: parent?.width ?? 0
            // The value is a field at the rail's end, wide enough for a size in pixels (owner, 2026-11-25).
            showValue: true
            endWidth: 14 * root.look.unit
            railWidth: width - endWidth - root.look.unit
            text: root.field.label
            from: root.field.min
            to: root.field.max
            stepSize: Number.isInteger(root.field.min) && Number.isInteger(root.field.max) ? 1 : 0.01
            // Round values pull the pointer in a wide whole range (sizes in pixels): 64s from 512 up, 16s from 128.
            snap: stepSize !== 1 ? 0 : root.field.max - root.field.min >= 512 ? 64 : root.field.max - root.field.min >= 128 ? 16 : 0
            value: Number(root.current ?? root.field.min)
            onMoved: v => {
                root.current = v
                root.edited(root.field.key, v)
            }
        }
    }
}
