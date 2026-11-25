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
    property url source // the picture; empty for the tone's blend
    property url video // its moving preview (a WebM): plays over the picture while `videoActive` (KitVideo keeps the budget)
    property bool videoActive: false
    readonly property url shownSource: look.params.heroStyle === "picture" ? source : "" // "tone" and "plain" never show it
    property string title
    property string subtitle
    property string status // the pill's text; empty hides it
    property color statusColor: look.tabColors.length > 2 ? look.tabColors[2] : look.accent // the pill's dot
    property bool statusTappable: false
    property bool showClose: false
    property real cornerRadius: look.params.areaLayout === "panes" ? 0 : Math.min(look.params.cornerHero * look.unit, 0.3 * implicitHeight)
    property real minHeight: 0
    // The blend says which part of Fugue a header belongs to (docs/canvas-look.md "Drift kit", Tones): dusk is the
    // brand (Fugue, Apps), sea Templates, ember Results and media, moss Get ready and downloads, slate Settings. An
    // unknown name is dusk. A picture (`source`) covers the blend; duskBase and duskBlobs take a blend of a host's own.
    property string tone: "dusk"
    readonly property var toneBlend: look.tone(tone)
    property color duskBase: toneBlend.base
    property var duskBlobs: toneBlend.blobs
    // Small round buttons in the top corner (the app panel's pin and Open in Fugue): children of the hero, laid in a row at the
    // end edge; the title elides before them.
    default property alias actions: actionRow.data
    signal statusTapped
    signal closed

    readonly property bool rounded: cornerRadius > 0 && GraphicsInfo.api !== GraphicsInfo.Software
    readonly property color ink: "white"
    // The white title keeps 4.5:1 on a pale picture: the mean luminance of the picture (PictureLuma, read once from a small
    // copy) sets a uniform shade just strong enough that the darkened picture is at most 0.18 (white on 0.18 is 4.5:1).
    // The dusk blend and dark pictures need none.
    readonly property real pictureLuma: shownSource.toString() !== "" ? PictureLuma.meanOfFile(source, 0, 0, 1, 1) : -1
    readonly property real legibleShade: pictureLuma > 0.18 ? Math.min(0.85, 1 - 0.18 / pictureLuma) : 0
    // The picture and its shade: what the frosted pills and buttons over it blur (KitFrost).
    readonly property Item backdropItem: art
    readonly property real edge: 3.5 * look.unit
    // How much of the top right the pill and the button take, so a long title elides before them.
    readonly property real cluster: (status !== "" ? pill.width + 2 * look.unit : 0) + (showClose ? close.width + look.unit : 0)
                                    + (actionRow.visibleChildren.length > 0 ? actionRow.width + look.unit : 0)

    implicitWidth: 93 * look.unit
    implicitHeight: Math.max(minHeight, titleBlock.height + 7 * look.unit)
    width: parent ? parent.width : implicitWidth
    height: implicitHeight
    clip: !rounded // the art reaches below the content and fades there; without the shader it is square and clipped
    Accessible.role: Accessible.Grouping
    Accessible.name: title

    // The picture and its shade, one item, drawn rounded by the thumbnail shader when it can.
    Item {
        id: art
        width: hero.width
        readonly property real overlap: hero.rounded ? hero.look.params.heroOverlap * hero.look.unit : 0
        height: hero.height + overlap
        visible: !hero.rounded
        layer.enabled: hero.rounded // the shader reads it as a texture
        Rectangle {
            anchors.fill: parent
            visible: hero.shownSource.toString() === ""
            color: hero.duskBase
            Repeater {
                model: hero.look.params.heroStyle === "plain" ? 0 : 3
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
            visible: hero.shownSource.toString() !== ""
            source: hero.shownSource
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize.width: 2 * hero.width
        }
        KitVideo {
            objectName: "heroVideo"
            anchors.fill: parent
            look: hero.look
            visible: hero.shownSource.toString() !== ""
            source: hero.video
            active: hero.videoActive && hero.shownSource.toString() !== ""
            fillMode: Image.PreserveAspectCrop
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            y: hero.rounded ? 0 : hero.height - height
            height: hero.rounded ? art.height : Math.min(hero.height * 0.6, 17.5 * hero.look.unit)
            // The title's shade: a little at the top, most at 60%, clearing toward the foot so no dark band shows (the software
            // renderer, which cannot fade the art, keeps the shade strong at the foot).
            gradient: hero.rounded ? roundedShade : footShade
            Gradient {
                id: roundedShade
                GradientStop { position: 0; color: Qt.rgba(0, 0, 0, hero.look.params.heroShadeTop) }
                GradientStop { position: hero.look.params.heroShadeAt; color: Qt.rgba(0, 0, 0, hero.look.light ? hero.look.params.heroShadeMidLight : hero.look.params.heroShadeMid) }
                GradientStop { position: 1; color: Qt.rgba(0, 0, 0, 0) }
            }
            Gradient {
                id: footShade
                GradientStop { position: 0; color: "transparent" }
                GradientStop { position: 1; color: hero.look.kitHeroShade }
            }
        }
        Rectangle {
            objectName: "heroLegibleShade"
            anchors.fill: parent
            visible: hero.legibleShade > 0
            color: Qt.rgba(0, 0, 0, hero.legibleShade)
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
            readonly property real fadeFrom: art.height * hero.look.params.heroFadeStart
            readonly property real fadeTo: art.height
            fragmentShader: "qrc:/canvas/shaders/hero.frag.qsb"
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
        color: "transparent"
        activeFocusOnTab: hero.statusTappable
        border.width: activeFocus && FocusInput.keyboard ? 2 : 0
        border.color: hero.look.accent
        Accessible.role: hero.statusTappable ? Accessible.Button : Accessible.StaticText
        Accessible.name: hero.status
        Accessible.onPressAction: hero.statusTapped()
        KitFrost {
            look: hero.look
            backdrop: hero.backdropItem
            backdropSource: hero.shownSource
            z: -1
            hovered: pillHover.hovered && hero.statusTappable
            pressed: pillTap.pressed
        }
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
        color: "transparent"
        activeFocusOnTab: true
        border.width: activeFocus && FocusInput.keyboard ? 2 : 0
        border.color: hero.look.accent
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Close")
        Accessible.onPressAction: hero.closed()
        KitFrost {
            look: hero.look
            backdrop: hero.backdropItem
            backdropSource: hero.shownSource
            z: -1
            hovered: closeHover.hovered
            pressed: closeTap.pressed
        }
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
    Row {
        id: actionRow
        x: (hero.showClose ? close.x - hero.look.unit : hero.width - hero.edge) - width
        y: 3 * hero.look.unit
        spacing: hero.look.unit
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
            readonly property bool serif: hero.look.params.titleFont === "serif"
            font.family: serif ? (hero.look.serifFont !== "" ? hero.look.serifFont : "Noto Serif") : Application.font.family
            font.pixelSize: Math.round(hero.look.textSize * 8 / 3)
            // The bundled Fraunces is variable with its default instance at weight 900: the calm regular cut is asked for on
            // its weight axis (font.weight alone does not move a variable font's default instance).
            font.weight: serif ? Font.Normal : Font.DemiBold
            font.variableAxes: serif ? ({ "wght": 420 }) : ({})
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
