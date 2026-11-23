pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../canvas"

// One media item, large (#133): an image fitted on the well color, a video or a sound in the
// player of #116 (MediaView). It knows nothing of lists: give it an `item` (a row of the browser's
// `items`: kind, url, title) and it shows it. The player is made only for a video or a sound and
// holds its decoder only while this is shown, so a browser of a hundred clips costs one.
//
// An image of a known size is shown in CompareView's single mode: the wheel zooms about the pointer,
// a drag pans a zoomed picture, `fit()`, `actualSize()` and `zoomBy()` do the same from a key or a
// button. With `compareWith` (another image of the list) the same viewer compares the two (`compareMode`:
// slider, slider_horizontal, side_by_side, flip or difference is not offered), A being `item`.
//
// Input:  look, item (or null), compareWith (an item or null), compareMode, expandable (the player's
//         large-view button)
// Output: pressed (a press on the picture), activated (a double click), expandRequested
Item {
    id: root
    required property CanvasLook look
    property var item: null
    property bool expandable: false
    property var compareWith: null
    property string compareMode: "slider"
    signal pressed
    signal activated
    signal expandRequested

    readonly property string kind: item?.kind ?? ""
    readonly property alias player: playerLoader.item
    // The zoomable picture; null for a video, a sound or an image whose size is unknown.
    readonly property var viewer: viewerLoader.item
    readonly property bool zoomable: viewer !== null && viewer !== undefined
    function fit() { viewer?.fit() }
    function actualSize() { viewer?.actualSize() }
    function zoomBy(factor) { viewer?.zoomAt(viewer.width / 2, viewer.height / 2, factor) }
    onItemChanged: viewer?.fit()

    Rectangle {
        anchors.fill: parent
        radius: root.look.thumbCorner
        color: root.look.well
    }
    Loader {
        id: viewerLoader
        anchors.fill: parent
        active: root.kind === "image" && (root.item?.width ?? 0) > 0
        sourceComponent: CompareView {
            objectName: "mediaViewer"
            look: root.look
            mode: root.compareWith ? root.compareMode : "single"
            showZoom: true
            sourceA: root.item?.url ?? ""
            sourceB: root.compareWith?.url ?? ""
            sizeA: Qt.size(root.item?.width ?? 0, root.item?.height ?? 0)
            sizeB: Qt.size(root.compareWith?.width ?? 0, root.compareWith?.height ?? 0)
            labelA: root.item?.title ?? "A"
            labelB: root.compareWith?.title ?? "B"
            background: "transparent"
            onPressed: root.pressed()
            onActivated: root.activated()
        }
    }
    Image {
        visible: root.kind === "image" && !root.zoomable
        anchors.fill: parent
        anchors.margins: 1
        source: root.kind === "image" ? root.item.url : ""
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: false
        mipmap: true
        // Never more than the screen needs.
        sourceSize.width: 2048
        sourceSize.height: 2048
    }
    Loader {
        id: playerLoader
        anchors.fill: parent
        active: root.kind === "video" || root.kind === "audio"
        sourceComponent: MediaView {
            look: root.look
            source: root.item?.url ?? ""
            kind: root.kind
            label: root.item?.title ?? ""
            expandable: root.expandable
            onPressed: root.pressed()
            onActivated: root.activated()
            onExpandRequested: root.expandRequested()
        }
    }
    MouseArea {
        anchors.fill: parent
        enabled: root.kind === "image" && !root.zoomable
        onPressed: root.pressed()
        onDoubleClicked: root.activated()
    }
    Text {
        visible: root.item === null
        anchors.centerIn: parent
        text: qsTr("Nothing to show")
        color: root.look.label
        font.pixelSize: root.look.labelSize
    }
}
