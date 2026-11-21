pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../canvas"

// One media item, large (#133): an image fitted on the well color, a video or a sound in the
// player of #116 (MediaView). It knows nothing of lists: give it an `item` (a row of the browser's
// `items`: kind, url, title) and it shows it. The player is made only for a video or a sound and
// holds its decoder only while this is shown, so a browser of a hundred clips costs one.
//
// Input:  look, item (or null), expandable (the player's large-view button)
// Output: pressed (a press on the picture), activated (a double click), expandRequested
Item {
    id: root
    required property CanvasLook look
    property var item: null
    property bool expandable: false
    signal pressed
    signal activated
    signal expandRequested

    readonly property string kind: item?.kind ?? ""
    readonly property alias player: playerLoader.item

    Rectangle {
        anchors.fill: parent
        radius: root.look.thumbCorner
        color: root.look.well
    }
    Image {
        visible: root.kind === "image"
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
        enabled: root.kind === "image"
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
