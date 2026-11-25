pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../kit"

// A choice among a few values (the kit's chooser, docs/canvas-look.md 20.9), one of them current. A short list of short texts is one
// KitSegmented (recessed glass track, a raised piece in the window's tone that slides to the choice); a longer list or longer texts
// are a row of raised KitChips that wraps, the chosen one in the tone. Either way each option is named <name>-<value>.
// Its own file, not an inline component of SettingsParts, so it can be Bound.
Item {
    id: choices
    property var options: [] // {value, text}
    property var current
    property string name // objectName prefix: <name>-<value>
    signal picked(var value)
    readonly property real maxSegmented: 96 * App.theme.look.unit // wider than this and the options wrap as chips
    readonly property bool segmented: options.length >= 2 && options.length <= 4 && options.every(o => String(o.text).length <= 22)
                                      && segment.implicitWidth <= Math.max(maxSegmented, 0)
    implicitWidth: segmented ? segment.implicitWidth : chips.implicitWidth
    implicitHeight: segmented ? segment.implicitHeight : chips.implicitHeight

    KitSegmented {
        id: segment
        visible: choices.segmented
        text: choices.name
        options: choices.options.map(o => o.text)
        optionNames: choices.options.map(o => choices.name + "-" + o.value)
        currentIndex: Math.max(-1, choices.options.findIndex(o => o.value === choices.current))
        onMoved: index => choices.picked(choices.options[index].value)
    }
    Flow {
        id: chips
        visible: !choices.segmented
        width: choices.width
        spacing: App.theme.look.unit
        Repeater {
            model: choices.segmented ? [] : choices.options
            delegate: KitChip {
                required property var modelData
                objectName: choices.name + "-" + modelData.value
                text: modelData.text
                pill: true
                toned: true
                current: modelData.value === choices.current
                Accessible.checkable: true
                Accessible.checked: current
                onTapped: choices.picked(modelData.value)
            }
        }
    }
}
