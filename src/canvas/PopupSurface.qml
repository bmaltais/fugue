import QtQuick
import Fugue
import "../kit"

// The surface of a popup, the `?` overlay and a dialog (docs/canvas-look.md 7, level 3; 20.21): a calm card of the Drift kit
// (KitPanel: the popup fill with a faint tint of `tone`, a hairline edge, the kit's card corner, so a dialog and a popover share the
// panel's corner) and the level 3 shadow. Use it as a Popup's background.
Item {
    id: surface
    required property CanvasLook look // App.theme.look
    property string tone: "slate" // what the popup is for (docs/canvas-look.md 20.1, 4); a menu or a question is the quiet slate

    Shadow {
        target: box
        level: 3
        look: surface.look
    }
    KitPanel {
        id: box
        look: surface.look
        tone: surface.tone
        calm: true
        anchors.fill: parent
    }
}
