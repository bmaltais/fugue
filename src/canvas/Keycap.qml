import QtQuick
import Fugue

// A key as the search footer and the `?` overlay show it (docs/canvas-look.md 13): at least
// 5u square, a 1 px edge in hairline-strong with a 2 px bottom edge, the node body's fill.
Rectangle {
    id: cap
    required property CanvasLook look // App.theme.look
    property string text
    property bool glyph: false // text is a Lucide glyph name

    implicitWidth: Math.max(5 * look.unit, label.implicitWidth + 2.5 * look.unit)
    implicitHeight: 5 * look.unit
    radius: Math.min(look.popupCorner, 4)
    color: look.hairlineStrong
    Rectangle {
        x: 1
        y: 1
        width: parent.width - 2
        height: parent.height - 3
        radius: Math.max(0, parent.radius - 1)
        color: cap.look.body
    }
    Text {
        id: label
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -1
        text: cap.glyph ? cap.look.glyph(cap.text) : cap.text
        font.family: cap.glyph ? cap.look.iconFont : Application.font.family
        font.pixelSize: cap.glyph ? 12 : cap.look.labelSize
        font.weight: Font.Medium
        color: cap.look.text
    }
}
