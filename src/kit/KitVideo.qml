pragma ComponentBehavior: Bound
import QtQuick
import QtMultimedia
import Fugue

// A moving picture of the Drift kit (docs/canvas-look.md 20.19): a still (`poster`) that is always there, and over it a muted,
// looping clip (`source`, a WebM, template.json's `preview`, spec 11.10) that plays only while the owner says it may (`active`:
// the card is hovered or in view, the open app's header) and the window is focused. The budget of 20.17 is kept here, not by each
// host:
//   - a clip that never plays is never opened: the player gets its source the first time it is wanted, not before;
//   - when it is not wanted the player is stopped (no decoding, no frames, the scene draws nothing), and the poster shows;
//   - reduced motion (the `motion` look parameter) and a clip that fails to play leave the poster alone, always;
//   - it measures nothing: the frosted ink of the controls over it reads the still, as before (KitFrost, PictureLuma).
// `playing` is true only while frames are being shown. The clip fades over the poster in 2 x the colour duration (none under
// reduced motion), so a card does not flash when it starts.
Item {
    id: root
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property url source // the clip; empty for a still only
    property url poster // the still shown at rest and until the first frame
    property bool active: false // the owner's wish: visible enough to play
    property int fillMode: Image.PreserveAspectCrop
    property int posterWidth: 512 // the poster's decoded width

    readonly property bool reduced: look.params.motion === "reduced"
    readonly property bool windowActive: Window.active
    readonly property bool wanted: active && source.toString() !== "" && !reduced && windowActive && !failed
    readonly property bool failed: player.error !== MediaPlayer.NoError
    readonly property bool playing: player.playbackState === MediaPlayer.PlayingState && player.position > 0
    // Whether the clip was ever wanted: the player gets its source then, not before (an idle player costs nothing).
    property bool made: false
    function sync() {
        if (wanted) {
            made = true
            player.play()
        } else {
            player.stop()
        }
    }
    onWantedChanged: sync()
    Component.onCompleted: sync()

    Image {
        anchors.fill: parent
        visible: source.toString() !== ""
        source: root.poster
        fillMode: root.fillMode
        asynchronous: true
        sourceSize.width: root.posterWidth
    }
    VideoOutput {
        id: output
        anchors.fill: parent
        fillMode: root.fillMode === Image.PreserveAspectCrop ? VideoOutput.PreserveAspectCrop : VideoOutput.PreserveAspectFit
        opacity: root.playing ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 2 * root.look.params.durColor } }
    }
    MediaPlayer {
        id: player
        objectName: "kitVideoPlayer"
        source: root.made ? root.source : ""
        videoOutput: output
        loops: MediaPlayer.Infinite // no audio output is set: it is muted
    }
}
