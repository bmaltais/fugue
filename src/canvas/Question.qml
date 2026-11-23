import QtQuick
import QtQuick.Controls
import Fugue

// A small modal question over the window (group, Frame and rename questions, Save before
// closing). It closes at once, with no fade, so the key pressed right after it reaches the canvas
// (#74): Material's 220 ms exit kept the window's shortcuts asleep meanwhile, as menus once did.
// Its answers are ThemedButtons (ViewButton, D4) in a QuestionAnswers row (#235). Right to left its content mirrors, as Settings and Projects do (#228): a popup lives in the
// overlay, outside the window's mirrored root, so it sets the mirroring on its own content.
// It reads App.theme.look directly, not its own `look` property, in the bindings that run while it is created: with
// Qt's sliced GC (QV4_GC_TIMELIMIT) a collection in the middle of a window's creation made `look` read null there (#320).
Popup {
    id: question
    readonly property CanvasLook look: App.theme.look
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 6 * App.theme.look.unit
    // The window's text size, not Material's 14px, so a question reads as Settings and Projects do (#235).
    font.pixelSize: App.theme.look.textSize
    // The overlay's width, not the parent's: a question is declared inside a zero-size host Item (a dialogs holder), whose
    // width made this negative and the surface vanish (the frame and padding were lost, the answers jammed on the text).
    width: Math.min((Overlay.overlay ? Overlay.overlay.width : 520) - 12 * App.theme.look.unit, 520)
    exit: null
    Overlay.modal: Rectangle { color: App.theme.look.scrim }
    background: PopupSurface { look: App.theme.look }
    Binding {
        target: question.contentItem
        property: "LayoutMirroring.enabled"
        value: Application.layoutDirection === Qt.RightToLeft
    }
    Binding {
        target: question.contentItem
        property: "LayoutMirroring.childrenInherit"
        value: true
    }
}
