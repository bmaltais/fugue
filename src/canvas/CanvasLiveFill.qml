pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// The live step frame of a canvas action's run, inside the selection (docs/canvas-actions-design.md, "Live preview in the
// selection"): the Sampler's newest frame is a picture of the padded context box the Region cut, so it is drawn on that
// box (canvas pixels, mapped by the view's origin and zoom) and clipped to the selection. It lies above the picture, under
// the selection's frame and the task bar's shimmer. It is not interactive. The first frame replaces nothing but the quiet
// picture; when a run ends well it fades over 200 ms (the new layer is already there), else it is gone at once. The last
// frame is kept while it fades.
Item {
    id: fill
    objectName: "canvasLiveFill"
    required property var actions // CanvasActions (App.canvasActions)
    required property string canvasId
    property real originX
    property real originY
    property real zoom: 1
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    readonly property bool active: actions !== null && actions.canvas === canvasId && actions.liveFrame !== ""
    property string frame
    property var box: ({x: 0, y: 0, width: 0, height: 0})
    property var clipBox: ({x: 0, y: 0, width: 0, height: 0})
    property bool fade: false
    function take() {
        if (!active)
            return
        frame = actions.liveFrame
        box = actions.liveBox
        clipBox = actions.liveClip
        fade = false
    }
    onActiveChanged: {
        if (active)
            take()
        else
            fade = actions !== null && actions.liveFade
    }
    Connections {
        target: fill.actions
        function onLiveChanged() { fill.take() } // a newer frame while one shows
    }
    Component.onCompleted: take()

    enabled: false // pointer events pass through
    opacity: active ? 1 : 0
    visible: active || opacity > 0
    Behavior on opacity { NumberAnimation { duration: fill.fade ? 200 : 0; easing.type: Easing.OutCubic } }

    Item {
        x: fill.originX + fill.clipBox.x * fill.zoom
        y: fill.originY + fill.clipBox.y * fill.zoom
        width: fill.clipBox.width * fill.zoom
        height: fill.clipBox.height * fill.zoom
        clip: true
        Image {
            x: (fill.box.x - fill.clipBox.x) * fill.zoom
            y: (fill.box.y - fill.clipBox.y) * fill.zoom
            width: fill.box.width * fill.zoom
            height: fill.box.height * fill.zoom
            source: fill.frame
            fillMode: Image.Stretch // the model-size frame is the context box at another scale
            asynchronous: true
            cache: false
            smooth: true
        }
    }
}
