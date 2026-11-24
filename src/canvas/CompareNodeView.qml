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
    // The file of a copy, for the face once the copy would be stretched (#371); empty where the picture already is the file.
    function file(which) {
        const p = compare[which]
        return p && !full && p.fileUrl ? p.fileUrl : ""
    }
    fileA: file("a")
    fileB: file("b")
    fileDifference: file("difference")
    // Decided on a change, not bound, because the rule remembers its last answer (CompareDetail.useFile).
    // By the wider picture: B may be much larger than A (a SeedVR2 restore against its 512 px original), and
    // both are drawn in A's frame, so the frame's width on screen over that picture's width is its scale.
    function refreshDetail() {
        const wide = Math.max(sizeA.width, sizeB.width)
        detail = !full && sizeA.width > 0 && wide > 0
                 && CompareDetail.useFile(imageScale * sizeA.width / wide, wide, detail)
    }
    onImageScaleChanged: refreshDetail()
    onZoomChanged: refreshDetail()
    onActualChanged: refreshDetail()
    onActualBChanged: refreshDetail()
    onSizeAChanged: refreshDetail()
    onSizeBChanged: refreshDetail()
    sourceA: picture("a")
    sourceB: picture("b")
    sourceDifference: picture("difference")
    sizeA: compare.a ? Qt.size(compare.a.width, compare.a.height) : Qt.size(0, 0)
    sizeB: compare.b ? Qt.size(compare.b.width, compare.b.height) : Qt.size(0, 0)
    labelA: compare.labelA || "A"
    labelB: compare.labelB || "B"

    // Hold-to-peek (#420): Z held shows 100% of the larger picture at the pointer; the model keeps the view to give back.
    // Called by the Graph's and the large view's keys, which ignore autorepeat; `peeking` makes a repeat harmless anyway.
    property bool peeking: false
    property real peekedAt: 0
    function peekDown() {
        if (peeking || single)
            return
        peeking = true
        peekedAt = Date.now()
        canvasModel.comparePeekBegin(nodeId, pointerImagePoint())
    }
    // Z released, or the keys lost (focus, window): a long hold gives the view back, a tap is the `1` key.
    function peekUp(cancel) {
        if (!peeking)
            return
        peeking = false
        canvasModel.comparePeekEnd(nodeId, cancel ? 1e9 : Date.now() - peekedAt)
    }

    onViewStateChanged: apply(viewState)
    Component.onCompleted: { apply(viewState); refreshDetail() }
    onViewChanged: current => canvasModel.setCompareView(nodeId, current)
}
