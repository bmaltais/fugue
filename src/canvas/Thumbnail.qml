pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue

// A node's last image (docs/canvas-look.md 4.6): fitted on the well color, a 1 px outline
// inside, its size in a chip at the bottom left. Loaded off the main thread at no more than
// twice its size on screen. Its corner is the node's minus its 6 px inset, so square on square
// themes; a rounded corner is cut by a small shader (never a layer or a mask per thumbnail),
// and on the software renderer, which has no shaders, it stays square.
Item {
    id: thumb
    property alias source: image.source
    property string info
    property string file // the file a Save Image wrote; empty for any other node
    property string text // shown in the image's place: a text or number result, a label
    required property CanvasLook look
    property bool full: true // the info chip only at the full zoom tier

    readonly property bool rounded: look.thumbCorner > 0 && GraphicsInfo.api !== GraphicsInfo.Software

    Rectangle {
        anchors.fill: parent
        radius: thumb.look.thumbCorner
        color: thumb.look.well
    }
    Image {
        id: image
        anchors.fill: parent
        visible: !thumb.rounded
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: false
        mipmap: true
        sourceSize.width: Math.min(512, 2 * thumb.width)
    }
    Loader {
        active: thumb.rounded
        x: (thumb.width - image.paintedWidth) / 2
        y: (thumb.height - image.paintedHeight) / 2
        width: image.paintedWidth
        height: image.paintedHeight
        sourceComponent: ShaderEffect {
            readonly property variant source: image
            readonly property size size: Qt.size(width, height)
            readonly property real radius: thumb.look.thumbCorner
            fragmentShader: "qrc:/canvas/shaders/thumbnail.frag.qsb"
        }
    }
    Text {
        visible: thumb.text !== ""
        x: Math.round(2 * thumb.look.unit)
        y: x
        width: parent.width - 2 * x
        height: parent.height - 2 * x
        text: thumb.full ? thumb.text : ""
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        elide: Text.ElideRight
        color: thumb.look.label
        font.pixelSize: thumb.look.labelSize
        font.features: { "tnum": 1 }
    }
    Rectangle {
        anchors.fill: parent
        radius: thumb.look.thumbCorner
        color: "transparent"
        border.width: 1
        border.color: thumb.look.light ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(1, 1, 1, 0.1)
    }
    // The chip sits on the image, not on the theme: white on black. It never passes the
    // picture: the file name (Save Image only) is elided in the middle, the extension kept, and
    // the size stays whole.
    Rectangle {
        id: chip
        objectName: "infoChip"
        visible: thumb.full && (thumb.info !== "" || thumb.file !== "")
        x: Math.round(1.5 * thumb.look.unit)
        y: parent.height - height - x
        readonly property real pad: 1.5 * thumb.look.unit
        width: Math.min(thumb.width - 2 * x, fileText.implicitWidth + sizeText.implicitWidth + 2 * pad)
        height: sizeText.implicitHeight + thumb.look.unit
        radius: thumb.look.smallCorner
        color: Qt.rgba(0, 0, 0, 0.6)
        clip: true
        Text {
            id: sizeText
            objectName: "infoSize"
            anchors.right: parent.right
            anchors.rightMargin: chip.pad
            anchors.verticalCenter: parent.verticalCenter
            text: (thumb.file !== "" && thumb.info !== "" ? "\u00b7 " : "") + thumb.info
            textFormat: Text.PlainText
            color: "white"
            font.pixelSize: thumb.look.captionSize
            font.features: { "tnum": 1 }
        }
        Text {
            id: fileText
            objectName: "infoFile"
            x: chip.pad
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(0, thumb.width - 2 * chip.x - sizeText.implicitWidth - 2 * chip.pad - thumb.look.unit))
            text: thumb.file
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            color: "white"
            font.pixelSize: thumb.look.captionSize
            font.features: { "tnum": 1 }
        }
        HoverHandler { id: chipHover }
        ToolTip.visible: chipHover.hovered && thumb.file !== ""
        ToolTip.delay: App.theme.look.tooltipDelay
        ToolTip.text: thumb.file
    }
}
