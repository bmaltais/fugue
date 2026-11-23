pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// The live frame of a running node whose picture the mode hides (#297): drawn in the canvas's
// world just under the card, as wide as it, above every card, never in it. The card's rect and
// the wires do not move for it ("a live frame never changes layout"). It is not interactive
// (clicks pass through), and fades out over 200 ms when the node stops running or its result
// comes. The last frame is kept while it fades.
Item {
    id: live
    objectName: "liveFrame"
    required property var source // the node's row (FugueCanvas.qml's Instantiator): liveFrame, liveAspect, posX, posY, nodeWidth, nodeHeight
    required property CanvasLook look
    signal gone

    readonly property string liveFrame: source.liveFrame
    property string frame
    property real aspect: 1
    onLiveFrameChanged: {
        if (liveFrame !== "") {
            frame = liveFrame
            aspect = source.liveAspect
        }
    }
    Component.onCompleted: {
        if (liveFrame !== "") {
            frame = liveFrame
            aspect = source.liveAspect
        }
    }
    // Made when a frame comes, gone once it has faded: a node without a frame has no item at all.
    onActiveChanged: if (!active && opacity === 0) gone()
    onOpacityChanged: if (!active && opacity === 0) gone()

    readonly property bool active: liveFrame !== ""
    readonly property real gap: Math.round(look.unit)
    // The card's rule for a picture: 0.5 to 1 of its width.
    x: source.posX
    y: source.posY + source.nodeHeight + gap
    width: source.nodeWidth
    height: Math.round(width * Math.min(Math.max(aspect, 0.5), 1))
    z: 1000000
    opacity: active ? 1 : 0
    visible: active || opacity > 0
    enabled: false // pointer events pass through
    Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }

    Rectangle {
        anchors.fill: parent
        radius: live.look.corner
        color: live.look.body
        border.width: 1
        border.color: live.look.hairline
    }
    Thumbnail {
        x: Math.round(live.look.unit)
        y: x
        width: parent.width - 2 * x
        height: parent.height - 2 * x
        source: live.frame
        look: live.look
        full: false
    }
}
