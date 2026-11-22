pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "compareMemory.js" as CompareMemory

// The compare modes of CompareView (#62) as a row of buttons: one list for every compare (#228, the cohesion audit's C4)
// in Compare Images' large view, the Canvas view and Canvas history, the same five everywhere, in CompareView's order.
// It draws and reports: `mode` is the viewer's mode now (its button is marked), `picked` says which one was pressed (the
// host sets the viewer), and the pick is remembered for the next canvas compare (compareMemory.js). Difference needs the
// |A - B| picture (`hasDifference`). `accented: false` is the quiet marking a surface with a primary uses (D7).
// objectNames are `prefix` + the mode's id ("compareMode-slider_horizontal").
Row {
    id: row
    required property CanvasLook look
    required property string prefix
    property string mode
    property bool hasDifference: true
    property bool accented: true
    signal picked(string mode)
    spacing: look.unit

    // The mode a canvas compare opens in: the one last picked, Slider at first.
    function remembered() { return CompareMemory.remembered() }

    Repeater {
        model: [{id: "slider", text: qsTr("Slider")}, {id: "slider_horizontal", text: qsTr("Slider, horizontal")},
                {id: "side_by_side", text: qsTr("Side by side")}, {id: "flip", text: qsTr("Flip")},
                {id: "difference", text: qsTr("Difference")}]
        delegate: ViewButton {
            required property var modelData
            objectName: row.prefix + modelData.id
            look: row.look
            text: modelData.text
            accented: row.accented
            current: row.mode === modelData.id
            enabled: modelData.id !== "difference" || row.hasDifference
            onTapped: {
                CompareMemory.remember(modelData.id)
                row.picked(modelData.id)
            }
        }
    }
}
