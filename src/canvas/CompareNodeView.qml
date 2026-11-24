import QtQuick
import Fugue

// A Compare Images node's viewer: CompareView fed from the node's run result (`compare`, the
// canvas model's role) and its state (`viewState`, the model's compareView), writing every change
// back to the model, which the node's face, the large view and the keys share (#62). The
// glue; the drawing is CompareView, which knows nothing of nodes.
CompareView {
    id: root
    required property CanvasModel canvasModel
    required property string nodeId
    property var compare: ({})
    property var viewState: ({})
    property bool full: false // the files themselves (the large view), not the 512 px copies

    function picture(which) {
        const p = compare[which]
        return p ? (full && p.fileUrl ? p.fileUrl : p.url) : ""
    }
    // The file of a copy, for the face once zoomed in (#371); empty where the picture already is the file.
    function file(which) {
        const p = compare[which]
        return p && !full && p.fileUrl ? p.fileUrl : ""
    }
    fileA: file("a")
    fileB: file("b")
    fileDifference: file("difference")
    // Decided on a change, not bound, because the rule remembers its last answer (CompareDetail.useFile).
    function refreshDetail() {
        detail = !full && sizeA.width > 0
                 && CompareDetail.useFile(imageScale, sizeA.width, !actual && zoom <= 1.001, detail)
    }
    onImageScaleChanged: refreshDetail()
    onZoomChanged: refreshDetail()
    onActualChanged: refreshDetail()
    onSizeAChanged: refreshDetail()
    sourceA: picture("a")
    sourceB: picture("b")
    sourceDifference: picture("difference")
    sizeA: compare.a ? Qt.size(compare.a.width, compare.a.height) : Qt.size(0, 0)
    sizeB: compare.b ? Qt.size(compare.b.width, compare.b.height) : Qt.size(0, 0)
    labelA: compare.labelA || "A"
    labelB: compare.labelB || "B"

    onViewStateChanged: apply(viewState)
    Component.onCompleted: { apply(viewState); refreshDetail() }
    onViewChanged: current => canvasModel.setCompareView(nodeId, current)
}
