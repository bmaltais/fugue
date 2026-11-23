pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// The aspect ratios of a picture window's tools row: Free, Original, 1:1, 4:3, 3:2, 16:9 and Turn (the Crop window's and
// the region picker's, docs/ui-ergonomics.md, C10). `prefix` names the buttons (`<prefix>Ratio-<id>`, `<prefix>RatioTurn`);
// the chosen one is the quiet marking, OK being the window's one accent fill (D7). `picked(id)` says one was pressed;
// `turned` is the ratio Turn would pick ("" when there is none: Free, Original). It knows nothing of the picture.
Row {
    id: root
    required property CanvasLook look
    property string prefix
    property string ratioName: "free"
    property string turned
    signal picked(string id)
    spacing: look.unit

    Repeater {
        model: [{id: "free", text: qsTr("Free")}, {id: "source", text: qsTr("Original")}, {id: "1:1", text: "1:1"},
                {id: "4:3", text: "4:3"}, {id: "3:2", text: "3:2"}, {id: "16:9", text: "16:9"}]
        delegate: ViewButton {
            required property var modelData
            objectName: root.prefix + "Ratio-" + modelData.id
            look: root.look
            text: modelData.text
            accented: false
            current: root.ratioName === modelData.id
            onTapped: root.picked(modelData.id)
        }
    }
    ViewButton {
        objectName: root.prefix + "RatioTurn"
        look: root.look
        text: qsTr("Turn")
        enabled: root.turned !== ""
        onTapped: root.picked(root.turned)
    }
}
