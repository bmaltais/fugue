pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// A rounded picture of the Drift kit, still or moving (docs/canvas-look.md 20.19): a card's thumbnail. `source` is the still,
// `video` the clip that plays over it while `playing` is wished (KitVideo keeps the budget), cut to `radius` by the thumbnail
// shader when the renderer has one (the software renderer shows it square). Children are laid over it, not cut: a card's badges
// and frosted buttons. `backdropItem` is the picture for a frosted control over it (KitChip `backdrop`).
Item {
    id: picture
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property url source
    property url video
    property bool playing: false
    property real radius: look.params.cornerGroup * look.unit * 0.7
    default property alias overlay: over.data

    readonly property bool rounded: radius > 0 && GraphicsInfo.api !== GraphicsInfo.Software
    readonly property Item backdropItem: art
    readonly property bool videoPlaying: moving.playing

    Item {
        id: art
        anchors.fill: parent
        layer.enabled: picture.rounded // the shader reads it as a texture
        visible: !picture.rounded
        clip: !picture.rounded
        Rectangle { anchors.fill: parent; color: picture.look.kitWell }
        KitVideo {
            id: moving
            objectName: "kitPictureVideo"
            anchors.fill: parent
            look: picture.look
            poster: picture.source
            source: picture.video
            active: picture.playing
        }
    }
    Loader {
        anchors.fill: parent
        active: picture.rounded
        sourceComponent: ShaderEffect {
            readonly property variant source: art
            readonly property size size: Qt.size(width, height)
            readonly property real radius: picture.radius
            fragmentShader: "qrc:/canvas/shaders/thumbnail.frag.qsb"
        }
    }
    Item { id: over; anchors.fill: parent }
}
