import QtQuick
import Fugue

// A small static label of the Drift kit (docs/canvas-look.md 20.2): "Free tier", "Added". A 5u pill in the `kitChip` fill with the
// text in the `label` color, 11px, 2u of padding at each end. It is not a button: it takes no focus and no press, so a person never
// takes it for one (a chip, 7u, is the thing to press).
Rectangle {
    id: badge
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property string text
    implicitHeight: 5 * look.unit
    implicitWidth: label.implicitWidth + 4 * look.unit
    width: implicitWidth
    height: implicitHeight
    radius: height / 2
    color: look.kitChip
    Accessible.role: Accessible.StaticText
    Accessible.name: text
    Text {
        id: label
        anchors.centerIn: parent
        text: badge.text
        textFormat: Text.PlainText
        color: badge.look.label
        font.pixelSize: badge.look.labelSize
    }
}
