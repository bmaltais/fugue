pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// A row of choices, one of them current (the segmented control of canvas-look 4.4). Its own
// file, not an inline component of SettingsParts, so it can be Bound.
Flow {
    id: choices
    property var options: [] // {value, text}
    property var current
    property string name // objectName prefix: <name>-<value>
    signal picked(var value)
    spacing: App.theme.look.unit
    Repeater {
        model: choices.options
        delegate: SettingsParts.SmallButton {
            required property var modelData
            objectName: choices.name + "-" + modelData.value
            text: modelData.text
            highlighted: modelData.value === choices.current
            Accessible.checked: highlighted
            onClicked: choices.picked(modelData.value)
        }
    }
}
