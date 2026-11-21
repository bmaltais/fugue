import QtQuick
import Fugue

// Node and Frame coloring, the color picker's first caller (#52): node.color.custom opens it on
// the color the selection or the Frame has now (else the last one used, else the accent), and
// what is picked becomes their layout color, one undo step.
Item {
    id: root

    // The Frame the picked color goes to, or "" for the selected nodes.
    property string frame: ""

    ColorPickerPopup {
        id: popup
        onPicked: color => App.layoutColors.apply(color, root.frame)
    }
    readonly property alias popup: popup

    Connections {
        target: App.canvasModel
        function onColorPickRequested(frame) {
            if (!App.colorHistory || !App.layoutColors)
                return
            root.frame = frame
            popup.openWith(App.layoutColors.current(frame) || App.colorHistory.recent[0]
                           || App.colorHistory.normalize(String(App.theme.accent)) || "#ff8800")
        }
    }
}
