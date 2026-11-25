import QtQuick
import Fugue

// A bold small title above a KitGroup (docs/canvas-look.md "Drift kit"): "Colour", "Motion", "Shape". It is a heading
// for assistive tools. Sentence case, as the window's words rule says.
Text {
    id: section
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    width: parent ? parent.width : implicitWidth
    leftPadding: 2 * look.unit
    topPadding: 5 * look.unit
    bottomPadding: 1.5 * look.unit
    textFormat: Text.PlainText
    elide: Text.ElideRight
    color: look.text
    font.pixelSize: look.resultSize
    font.bold: true
    Accessible.role: Accessible.Heading
    Accessible.name: text
}
