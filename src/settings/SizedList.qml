pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue

// A list of things with a size, and an action on each: a row has a title, a line under it, its
// size, a bar of its share of the biggest, and one button. Generic: the Cache page lists kinds of
// cache with Clear, and anything else with a size and a verb can use it.
//   model: [{title, detail, size (text), bytes (number, for the bar), actionText, actionEnabled}]
// It only reports `actionClicked(index)`; what the action does is the page's.
ColumnLayout {
    id: list
    property var model: []
    property string name: "sized" // objectName prefix: <name>-<index>, <name>-action-<index>
    signal actionClicked(int index)
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property real biggest: model.reduce((most, row) => Math.max(most, row.bytes), 1)
    spacing: 2 * u

    Repeater {
        model: list.model
        delegate: Rectangle {
            id: row
            required property var modelData
            required property int index
            objectName: list.name + "-" + index
            Layout.fillWidth: true
            implicitHeight: rowLayout.implicitHeight + 6 * list.u
            radius: list.look.smallCorner
            color: "transparent"
            border.width: 1
            border.color: list.look.hairline
            Rectangle { // the share of the biggest, under the text
                x: 1
                y: parent.height - height - 1
                height: 2
                width: Math.max(0, (parent.width - 2) * row.modelData.bytes / list.biggest)
                color: list.look.accent
                opacity: 0.6
            }
            RowLayout {
                id: rowLayout
                x: 3 * list.u
                y: 3 * list.u
                width: row.width - 6 * list.u
                spacing: 2 * list.u
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: row.modelData.title
                        color: list.look.text
                        font.pixelSize: list.look.textSize
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: row.modelData.detail
                        color: list.look.label
                        font.pixelSize: list.look.labelSize
                    }
                }
                Text {
                    objectName: list.name + "-size-" + row.index
                    text: row.modelData.size
                    color: list.look.text
                    font.pixelSize: list.look.textSize
                    font.features: { "tnum": 1 }
                }
                SettingsParts.SmallButton {
                    objectName: list.name + "-action-" + row.index
                    visible: text !== ""
                    text: row.modelData.actionText
                    enabled: row.modelData.actionEnabled
                    opacity: enabled ? 1 : 0.5
                    onClicked: list.actionClicked(row.index)
                }
            }
        }
    }
}
