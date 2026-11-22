import QtQuick
import Fugue

// A question's answer, the window's one button (docs/ui-ergonomics.md D4, #235): a thin alias of ViewButton that keeps the
// names the dialogs use (`highlighted`, `clicked`), as the settings' SmallButton does. It is a Material Button no longer: that
// one drew its own 48px, fill-less look and a label in a size of its own. `highlighted` is the question's primary (the accent
// fill, one per question; in a destructive question it is the SAFE answer, D8); `danger` tints a destructive answer; `flat`
// is a link-like answer with no edge. Enter and Space press it, with the 2px focus ring.
ViewButton {
    property bool highlighted: false
    signal clicked()
    look: App.theme.look
    bordered: !flat
    primary: highlighted
    onTapped: clicked()
}
