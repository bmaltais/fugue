import QtQuick
import QtQuick.Controls
import Fugue

// A TextField whose selection is drawn in the theme's accent with its readable foreground (#74):
// Material's own is white on the accent, which a pale accent makes unreadable.
TextField {
    selectionColor: App.theme.look.accent
    selectedTextColor: App.theme.look.accentForeground
}
