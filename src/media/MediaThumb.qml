import QtQuick
import Fugue
import "../canvas"

// One cell of the browser's grid or filmstrip (#133): the item's thumbnail fitted on the well
// color, or for a video or a sound a glyph and its name, with a ring while it is the current one.
// The picture is loaded only once the cell exists (the lists make cells for what is in view) and
// from the thumbnail cache (`item.thumb`), at the cell's size.
Item {
    id: cell
    required property CanvasLook look
    property var item: null
    property bool current: false
    property bool focused: false
    // False in a list row of something else (the queue's finished runs): no name under the glyph and
    // no pointer handling, so the row's own click is the only one.
    property bool compact: false
    signal tapped
    signal activated

    Rectangle {
        anchors.fill: parent
        radius: cell.look.thumbCorner
        color: cell.look.well
    }
    Image {
        anchors.fill: parent
        anchors.margins: 1
        visible: (cell.item?.thumb ?? "") !== ""
        source: cell.item?.thumb ?? ""
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: false
        sourceSize.width: 256
        sourceSize.height: 256
    }
    Column {
        visible: (cell.item?.thumb ?? "") === ""
        anchors.centerIn: parent
        width: parent.width - 2 * cell.look.unit
        spacing: cell.look.unit
        Glyph {
            anchors.horizontalCenter: parent.horizontalCenter
            look: cell.look
            name: cell.item?.kind === "video" ? "play" : cell.item?.kind === "audio" ? "volume-2" : "image"
            color: cell.look.label
            font.pixelSize: cell.compact ? 14 : 20
        }
        Text {
            visible: !cell.compact
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: cell.item?.title ?? ""
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            maximumLineCount: 2
            wrapMode: Text.WrapAnywhere
            color: cell.look.label
            font.pixelSize: cell.look.captionSize
        }
    }
    Rectangle {
        anchors.fill: parent
        radius: cell.look.thumbCorner
        color: "transparent"
        border.width: cell.current ? 2 : 1
        border.color: cell.current ? (cell.focused ? cell.look.accent : cell.look.label)
                                   : (cell.look.light ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(1, 1, 1, 0.1))
    }
    HoverHandler { enabled: !cell.compact; cursorShape: Qt.PointingHandCursor }
    TapHandler {
        enabled: !cell.compact
        onTapped: cell.tapped()
        onDoubleTapped: cell.activated()
    }
    Accessible.role: Accessible.ListItem
    Accessible.name: cell.item?.title ?? ""
    Accessible.selected: cell.current
}
