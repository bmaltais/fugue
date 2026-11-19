import QtQuick

// A node's last image (docs/canvas-look.md 4.6): fitted on the well color, a 1 px outline
// inside, its size in a chip at the bottom left. Loaded off the main thread at no more than
// twice its size on screen. Its corner is the node's minus its 6 px inset, so square on square
// themes; a rounded corner is cut by a small shader (never a layer or a mask per thumbnail),
// and on the software renderer, which has no shaders, it stays square.
Item {
    id: thumb
    property alias source: image.source
    property string info
    required property var look
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
    Rectangle {
        anchors.fill: parent
        radius: thumb.look.thumbCorner
        color: "transparent"
        border.width: 1
        border.color: thumb.look.light ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(1, 1, 1, 0.1)
    }
    // The chip sits on the image, not on the theme: white on black.
    Rectangle {
        visible: thumb.full && thumb.info !== ""
        x: Math.round(1.5 * thumb.look.unit)
        y: parent.height - height - x
        width: chipText.implicitWidth + 3 * thumb.look.unit
        height: chipText.implicitHeight + thumb.look.unit
        radius: thumb.look.smallCorner
        color: Qt.rgba(0, 0, 0, 0.6)
        Text {
            id: chipText
            anchors.centerIn: parent
            text: thumb.info
            textFormat: Text.PlainText
            color: "white"
            font.pixelSize: thumb.look.captionSize
            font.features: { "tnum": 1 }
        }
    }
}
