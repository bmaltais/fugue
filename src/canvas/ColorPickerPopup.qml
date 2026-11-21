import QtQuick
import QtQuick.Controls
import Fugue

// ColorPickerPane on a popup, centered in the window. Not tied to any caller: `openWith(color)`
// opens it started on a color, and `picked(color)` says what was confirmed; the popup closes
// then. Cancel, Escape or a press outside closes it with no `picked`. See ColorPickerPane for the rest.
Popup {
    id: root
    objectName: "colorPickerPopup"

    readonly property CanvasLook look: App.theme.look
    readonly property alias pane: colorPane
    signal picked(string color)

    function openWith(color) {
        colorPane.setColor(color)
        open()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    padding: 3 * look.unit
    modal: false
    focus: true
    // No fade or scale, as the menus have none (docs/canvas-look.md 15): it takes input at once.
    enter: null
    exit: null
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: colorPane.focusField()
    background: PopupSurface { look: root.look }

    contentItem: ColorPickerPane {
        id: colorPane
        onPicked: color => {
            root.close()
            root.picked(color)
        }
        onCanceled: root.close()
    }
}
