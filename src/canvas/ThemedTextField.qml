import QtQuick
import QtQuick.Controls
import Fugue

// A TextField whose selection is drawn in the theme's accent with its readable foreground (#74):
// Material's own is white on the accent, which a pale accent makes unreadable. Focused, the edge lifts to 1px of `label` at
// 45% (a field shows its caret, not the accent ring; owner 2026-11-25), where Material draws an accent underline.
TextField {
    id: control
    selectionColor: App.theme.look.accent
    selectedTextColor: App.theme.look.accentForeground
    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 40
        radius: App.theme.look.smallCorner
        color: App.theme.look.well
        border.width: 1
        border.color: control.activeFocus ? Qt.rgba(App.theme.look.label.r, App.theme.look.label.g, App.theme.look.label.b, 0.45)
                                          : App.theme.look.hairlineStrong
    }
}
