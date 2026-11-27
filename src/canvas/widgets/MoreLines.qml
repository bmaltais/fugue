import QtQuick
import ".."

// "+N lines" at the right of a multiline well's caption line while the well cuts its text (#715, canvas-look 4.4): what decides
// whether to open the editor. The lines are counted by a hidden copy of the text with no line limit (the well's own text stops at its
// limit), given at most `countCap` characters so a huge prompt is not laid out whole; past that the number says "N+". Made the first
// time the well is cut.
Item {
    id: more
    required property InputWidget host
    required property Text label // the well's text
    readonly property int countCap: 20000
    readonly property bool cut: host.cutShort
    readonly property int hidden: cut ? Math.max(0, counter.lineCount - label.lineCount) : 0
    readonly property bool capped: host.valueText.length > countCap
    anchors.fill: parent
    visible: cut && hidden > 0
    z: 1

    Text {
        id: counter
        visible: false
        width: more.label.width
        text: more.cut ? (more.capped ? more.host.valueText.slice(0, more.countCap) : more.host.valueText) : ""
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
