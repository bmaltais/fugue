import QtQuick
import QtQuick.Controls
import Fugue

// A small modal question over the window (group, Frame and rename questions, Save before
// closing). It closes at once, with no fade, so the key pressed right after it reaches the canvas
// (#74): Material's 220 ms exit kept the window's shortcuts asleep meanwhile, as menus once did.
Popup {
    readonly property CanvasLook look: App.theme.look
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 6 * look.unit
    width: Math.min((parent ? parent.width : 520) - 12 * look.unit, 520)
    exit: null
    Overlay.modal: Rectangle { color: App.theme.look.scrim }
    background: PopupSurface { look: App.theme.look }
}
