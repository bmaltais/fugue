import QtQuick

// A Lucide glyph (docs/canvas-look.md 16) by name, from the bundled icon font: it recolors
// through `color` and batches with the labels around it.
Text {
    required property var look // theme.look
    property string name

    text: name !== "" ? look.glyph(name) : ""
    font.family: look.iconFont
    font.pixelSize: 14
}
