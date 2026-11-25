import QtQuick
import Fugue
import "../canvas"

// One cell of the browser's grid or filmstrip (#133): the item's thumbnail fitted on the well
// color, or the glyph and the name when there is no picture (a file that cannot be read, a
// video or a sound with no ffmpeg), with a ring while it is the current one. A video's picture
// is a frame with a small play mark in a corner, a sound's its waveform with a speaker mark.
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
        radius: Math.min(cell.look.params.cornerGroup * cell.look.unit, 0.2 * Math.min(width, height))
        color: cell.look.well
    }
    Image {
        id: picture
        anchors.fill: parent
        anchors.margins: 1
        visible: status === Image.Ready
        source: cell.item?.thumb ?? ""
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: false
        sourceSize.width: 256
        sourceSize.height: 256
    }
    // A mark on a picture that is not a still: what it is, before the cell is opened.
    Glyph {
        visible: picture.status === Image.Ready && (cell.item?.kind ?? "image") !== "image"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: cell.look.unit / 2
        look: cell.look
        name: cell.item?.kind === "video" ? "play" : "volume-2"
        color: cell.look.label
        font.pixelSize: cell.compact ? 10 : 14
    }
    // A favorite wears a star in the upper right corner.
    Text {
        objectName: "favoriteMark"
        visible: cell.item?.favorite === true
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: cell.look.unit / 2
        text: "\u2605"
        color: cell.look.accent
        style: Text.Outline
        styleColor: Qt.rgba(0, 0, 0, 0.6)
        font.pixelSize: cell.compact ? 10 : 14
    }
    Column {
        // Until the picture is ready or failed, only the well: no glyph flashing under a thumbnail.
        visible: picture.status === Image.Error || picture.status === Image.Null
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
        radius: Math.min(cell.look.params.cornerGroup * cell.look.unit, 0.2 * Math.min(width, height))
        color: "transparent"
        border.width: cell.current ? 2 : 1
        border.color: cell.current ? (cell.focused && FocusInput.keyboard ? cell.look.accent : cell.look.label)
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
