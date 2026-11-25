import QtQuick
import Fugue
import "../canvas"

// The large view of one result of an app (#385): the full file in the viewer every large picture uses (CompareView in its
// single mode, #73), with Fit, 100% and Escape from LargeViewDialog. `openFor(result)` takes an AppView result.
LargeViewDialog {
    id: root
    objectName: "appResultDialog"
    name: "appResult"
    look: App.theme.look
    zoomable: view

    function openFor(result) {
        view.sourceA = result.full
        view.sizeA = Qt.size(result.width, result.height)
        view.labelA = result.label
        view.fit()
        open()
    }

    CompareView {
        id: view
        objectName: "appResultViewer"
        anchors.fill: parent
        look: root.look
        mode: "single"
        showZoom: true
    }
}
