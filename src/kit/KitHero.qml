pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import Fugue
import "../canvas"

// The header of a Drift-style card (docs/canvas-look.md "Drift kit"): a picture, or with none the "dusk" blend (the
// brand default: lilac, rose and blue blobs on a dark violet-grey), a serif title and a one-line subtitle at the top
// left, a darkening gradient toward the foot so they read on any picture, an optional status pill and an optional
// round close button at the top right. It is compact: its height is the title, the subtitle and modest padding (about
// 84px for a 32px title at text size 12), never a tall empty band; `minHeight` makes room when a page wants more
// picture (the Fugue header, 45u). Its top corners follow the card (`cornerRadius`); the foot is square, so a group can
// start right under it.
//
// The title is white over the picture on every theme (a literal over a picture, as CropFrame's), in Fraunces, which
// Fugue bundles (`look.serifFont`, SIL OFL), then Noto Serif. The dusk colors are the brand's, the same
// on every theme, being a picture. The pill and the button are 6u (24px) tall, the floor. Value in, change out:
// `status` and `showClose` come in, `statusTapped` and `closed` go out.
Item {
    id: hero
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property url source // the picture; empty for the theme's gradient
    property string title
    property string subtitle
    property string status // the pill's text; empty hides it
    property color statusColor: look.tabColors.length > 2 ? look.tabColors[2] : look.accent // the pill's dot
    property bool statusTappable: false
    property bool showClose: false
    property real cornerRadius: look.popupCorner + 4
    property real minHeight: 0
    property color duskBase: "#2a2533"
    property var duskBlobs: ["#b98bd6", "#e3828f", "#6f86d6"] // lilac, rose, blue
    signal statusTapped
    signal closed

    readonly property bool rounded: cornerRadius > 0 && GraphicsInfo.api !== GraphicsInfo.Software
    readonly property color ink: "white"
    readonly property real edge: 3.5 * look.unit
    // How much of the top right the pill and the button take, so a long title elides before them.
    readonly property real cluster: (status !== "" ? pill.width + 2 * look.unit : 0) + (showClose ? close.width + look.unit : 0)

    implicitWidth: 93 * look.unit
    implicitHeight: Math.max(minHeight, titleBlock.height + 7 * look.unit)
    width: parent ? parent.width : implicitWidth
    height: implicitHeight
    clip: true // the picture is rounded all round and taller by its corner; the clip squares the foot
    Accessible.role: Accessible.Grouping
    Accessible.name: title

    // The picture and its shade, one item, drawn rounded by the thumbnail shader when it can.
    Item {
        id: art
        width: hero.width
        height: hero.height + hero.cornerRadius
        visible: !hero.rounded
        layer.enabled: hero.rounded // the shader reads it as a texture
        Rectangle {
            anchors.fill: parent
            visible: hero.source.toString() === ""
            color: hero.duskBase
            Repeater {
                model: 3
                Shape {
                    id: blob
                    required property int index
                    readonly property real size: hero.width * [0.95, 0.8, 0.9][index]
                    readonly property color tone: hero.duskBlobs[index]
                    x: hero.width * [0.1, 0.5, 0.92][index] - size / 2
                    y: art.height * [0.2, 0.7, 0.15][index] - size / 2
                    width: size
                    height: size
                    ShapePath {
                        strokeColor: "transparent"
                        fillGradient: RadialGradient {
                            centerX: blob.size / 2; centerY: blob.size / 2
                            centerRadius: blob.size / 2; focalX: centerX; focalY: centerY
                            GradientStop { position: 0; color: Qt.rgba(blob.tone.r, blob.tone.g, blob.tone.b, 1.0) }
                            GradientStop { position: 1; color: Qt.rgba(blob.tone.r, blob.tone.g, blob.tone.b, 0) }
                        }
                        startX: 0; startY: 0
                        PathLine { x: blob.size; y: 0 }
                        PathLine { x: blob.size; y: blob.size }
                        PathLine { x: 0; y: blob.size }
                    }
                }
            }
        }
        Image {
            anchors.fill: parent
            visible: hero.source.toString() !== ""
            source: hero.source
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize.width: 2 * hero.width
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            y: hero.height - height
            height: Math.min(hero.height * 0.6, 17.5 * hero.look.unit)
            gradient: Gradient {
                GradientStop { position: 0; color: "transparent" }
                GradientStop { position: 1; color: hero.look.kitHeroShade }
            }
        }
    }
    Loader {
        active: hero.rounded
        width: art.width
        height: art.height
        sourceComponent: ShaderEffect {
            readonly property variant source: art
            readonly property size size: Qt.size(width, height)
            readonly property real radius: hero.cornerRadius
            fragmentShader: "qrc:/canvas/shaders/thumbnail.frag.qsb"
        }
    }

    Rectangle {
        id: pill
        visible: hero.status !== ""
        x: close.visible ? close.x - width - 2 * hero.look.unit : hero.width - width - hero.edge
        y: 3 * hero.look.unit
        height: 6 * hero.look.unit
        width: pillRow.implicitWidth + 4 * hero.look.unit
        radius: height / 2
        color: pillTap.pressed || pillHover.hovered && hero.statusTappable ? hero.look.kitHeroPillHover : hero.look.kitHeroPill
        activeFocusOnTab: hero.statusTappable
        border.width: activeFocus ? 2 : 0
        border.color: hero.look.accent
        Accessible.role: hero.statusTappable ? Accessible.Button : Accessible.StaticText
        Accessible.name: hero.status
        Accessible.onPressAction: hero.statusTapped()
        Row {
            id: pillRow
            anchors.centerIn: parent
            spacing: 1.5 * hero.look.unit
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 1.5 * hero.look.unit
                height: width
                radius: width / 2
                color: hero.statusColor
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: hero.status
                textFormat: Text.PlainText
                color: hero.ink
                font.pixelSize: hero.look.labelSize
                font.weight: Font.DemiBold
            }
        }
        TapHandler {
            id: pillTap
            enabled: hero.statusTappable
            gesturePolicy: TapHandler.WithinBounds
            onTapped: hero.statusTapped()
        }
        HoverHandler {
            id: pillHover
            cursorShape: hero.statusTappable ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
        Keys.onSpacePressed: if (hero.statusTappable) hero.statusTapped()
        Keys.onReturnPressed: if (hero.statusTappable) hero.statusTapped()
    }
    Rectangle {
        id: close
        visible: hero.showClose
        x: hero.width - width - hero.edge
        y: 3 * hero.look.unit
        width: 6 * hero.look.unit
        height: width
        radius: width / 2
        color: closeTap.pressed || closeHover.hovered ? hero.look.kitHeroPillHover : hero.look.kitHeroPill
        activeFocusOnTab: true
        border.width: activeFocus ? 2 : 0
        border.color: hero.look.accent
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Close")
        Accessible.onPressAction: hero.closed()
        Glyph {
            anchors.centerIn: parent
            look: hero.look
            name: "x"
            color: hero.ink
        }
        TapHandler {
            id: closeTap
            gesturePolicy: TapHandler.WithinBounds
            onTapped: hero.closed()
        }
        HoverHandler {
            id: closeHover
            cursorShape: Qt.PointingHandCursor
        }
        Keys.onSpacePressed: hero.closed()
        Keys.onReturnPressed: hero.closed()
    }
    Column {
        id: titleBlock
        x: 5 * hero.look.unit
        y: 4 * hero.look.unit
        width: hero.width - 2 * x - hero.cluster
        spacing: 0.5 * hero.look.unit
        Text {
            width: parent.width
            text: hero.title
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: hero.ink
            font.family: hero.look.serifFont !== "" ? hero.look.serifFont : "Noto Serif"
            font.pixelSize: Math.round(hero.look.textSize * 8 / 3)
        }
        Text {
            width: parent.width
            visible: hero.subtitle !== ""
            text: hero.subtitle
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Qt.rgba(1, 1, 1, 0.88)
            font.pixelSize: hero.look.textSize
        }
    }
}
