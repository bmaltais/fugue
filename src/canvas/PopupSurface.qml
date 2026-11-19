import QtQuick

// The surface of a popup, the `?` overlay and a dialog (docs/canvas-look.md 7, level 3): the
// popup fill, a hairline edge, the theme's corner, and the level 3 shadow. Use it as a
// Popup's background.
Item {
    id: surface
    required property var look // theme.look

    Shadow {
        target: box
        level: 3
        look: surface.look
    }
    Rectangle {
        id: box
        anchors.fill: parent
        radius: surface.look.popupCorner
        color: surface.look.popup
        border.width: 1
        border.color: surface.look.hairline
    }
}
