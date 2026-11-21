import QtQuick
import Fugue

// The large view of a Save Video, Save Audio or Preview result (#116): the same player as the node's
// face (MediaView), over the window in LargeViewDialog's frame (the bar, the close button, Escape).
// A player does not zoom, so the frame has no Fit and 100%. `openFor(node)` opens it on what the
// node last made and starts it; closing stops it.
LargeViewDialog {
    id: root
    objectName: "mediaDialog"
    name: "media"
    look: App.theme.look
    required property CanvasModel canvasModel
    property string nodeId
    readonly property alias viewer: player

    function openFor(node) {
        nodeId = node
        const media = canvasModel.mediaResult(node)
        player.kind = media.type || "video"
        player.label = (media.file || "") !== "" ? media.file : (media.label || canvasModel.nodeTitle(node))
        player.source = media.url || ""
        open()
    }
    onOpened: { player.forceActiveFocus(); player.play() }
    onClosed: { player.pause(); player.source = "" }

    MediaView {
        id: player
        objectName: "mediaViewer"
        anchors.fill: parent
        look: root.look
    }
}
