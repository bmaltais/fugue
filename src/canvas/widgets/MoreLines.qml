import QtQuick
import ".."

// "+N lines" at the right of a multiline well's caption line while the well cuts its text (#715, canvas-look 4.4): what decides
// whether to open the editor. The lines are counted by a hidden copy of the text with no line limit (the well's own text stops at its
// limit), given at most `countCap` characters so a huge prompt is not laid out whole; past that the number says "N+". Made the first
// time the well is cut. The count is made while the card is shown, and a moment after the text stopped changing (the text window
// changes it on every key): a hidden card, or a burst of keys, lays nothing out.
Item {
    id: more
    required property InputWidget host
    required property Text label // the well's text
    readonly property int countCap: 20000
    readonly property bool cut: host.cutShort
    // What is counted: the text as it stood when it last held still, and only while the card is on screen.
    property string counted: ""
    readonly property bool counting: cut && host.visible
    readonly property int hidden: cut ? Math.max(0, counter.lineCount - label.lineCount) : 0
    readonly property bool capped: host.valueText.length > countCap
    anchors.fill: parent
    visible: cut && hidden > 0
    z: 1

    function recount() {
        counted = host.valueText.length > countCap ? host.valueText.slice(0, countCap) : host.valueText
    }
    Connections {
        target: more.host
        function onValueTextChanged() { if (more.counting) settle.restart() }
    }
    onCountingChanged: if (counting) recount()
    Component.onCompleted: if (counting) recount()
    Timer {
        id: settle
        interval: 120
        onTriggered: more.recount()
    }
    Text {
        id: counter
        visible: false
        width: more.label.width
        text: more.counting ? more.counted : ""
        wrapMode: Text.Wrap
        lineHeightMode: Text.FixedHeight
        lineHeight: 16
        textFormat: Text.PlainText
        font.pixelSize: more.host.labelPx
    }
    Text {
        objectName: "widgetMoreLines"
        x: more.host.pad
        y: Math.round(1.5 * more.host.look.unit)
        width: more.width - 2 * more.host.pad - more.host.expandRoom
        height: 16
        horizontalAlignment: Text.AlignRight
        verticalAlignment: Text.AlignVCenter
        text: more.capped ? qsTr("+%1 lines").arg(more.hidden + "+") : qsTr("+%n lines", "", more.hidden)
        color: more.host.look.label
        font.pixelSize: more.host.look.labelSize
    }
}
