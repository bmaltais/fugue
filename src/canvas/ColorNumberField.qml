import QtQuick
import QtQuick.Layouts
import Fugue

// One channel of the color picker (ColorPickerPane): a short label and a whole-number field that
// takes only digits, and a number past `maximum` becomes `maximum`. `value` is what the field shows when it is not being
// typed in; `edited(n)` says a valid number was typed, as it is typed. Knows nothing of colors.
RowLayout {
    id: root

    readonly property CanvasLook look: App.theme.look
    required property string name  // "H", "S", "B", "R", "G", "B"
    property string accessibleName // spoken name of the field, "Hue" for "H"
    property int maximum: 255
    property int value: 0
    property string suffix: ""     // "°" or "%", shown after the number
    readonly property alias field: input
    signal edited(int number)
    signal accepted

    spacing: look.unit

    // What the field shows, unless the person is typing in it (then it is theirs).
    function sync(force) {
        if (force || !input.activeFocus)
            input.text = String(value)
    }
    onValueChanged: sync(false)
    Component.onCompleted: sync(true)

    Text {
        Layout.preferredWidth: 3 * root.look.unit
        text: root.name
        horizontalAlignment: Text.AlignRight
        color: root.look.label
        font.pixelSize: root.look.labelSize
        Accessible.ignored: true
    }
    ThemedTextField {
        id: input
        objectName: root.objectName + "-field"
        Layout.fillWidth: true
        Layout.preferredWidth: 11 * root.look.unit
        Layout.preferredHeight: 7 * root.look.unit
        font.pixelSize: root.look.labelSize
        horizontalAlignment: Text.AlignRight
        rightPadding: root.suffix !== "" ? 4.5 * root.look.unit : leftPadding
        selectByMouse: true
        inputMethodHints: Qt.ImhDigitsOnly
        validator: IntValidator { bottom: 0; top: root.maximum }
        Accessible.name: root.accessibleName !== "" ? root.accessibleName : root.name
        onTextEdited: {
            if (text === "")
                return
            const n = Math.min(root.maximum, parseInt(text)) // 999 in a 100 field is 100
            if (String(n) !== text && parseInt(text) > root.maximum)
                text = String(n)
            root.edited(n)
        }
        onAccepted: root.accepted()
        onActiveFocusChanged: if (!activeFocus) root.sync(true) // leaving: show the real value
        Text {
            visible: root.suffix !== ""
            anchors.right: parent.right
            anchors.rightMargin: root.look.unit
            anchors.verticalCenter: parent.verticalCenter
            text: root.suffix
            color: root.look.label
            font.pixelSize: root.look.labelSize
        }
    }
}
