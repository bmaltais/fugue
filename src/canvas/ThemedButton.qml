import QtQuick
import QtQuick.Controls
import Fugue

// A Button whose label reads on its fill (#74): Material draws a highlighted button's label in
// white whatever the accent, unreadable on a pale one; here it is the accent's own foreground.
Button {
    id: control
    contentItem: Text {
        text: control.text
        font: control.font
        elide: Text.ElideRight
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: !control.enabled ? App.theme.look.label
             : control.highlighted ? App.theme.look.accentForeground : App.theme.foreground
    }
}
