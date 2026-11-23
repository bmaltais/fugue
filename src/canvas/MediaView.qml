pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtMultimedia
import Fugue

// A player for one video or one sound (#116). It knows nothing of the canvas, a node or a run: give
// it a `source` and the `kind` of what it is, and it plays it; the node's face and the large view
// each make one, and the same bar serves both. The bar is the one place a person acts: play and
// pause, the seek track, the time, the volume (a small slider; the wheel over it and Up and Down also
// change it), mute and, when the caller allows it, a button that asks for the
// large view. The picture itself is for looking at: a double click on it is `activated`, a press
// on it is `pressed` (the node's face selects the node).
//
// Input:  source (a file URL), kind ("video" or "audio"), label (what stands where the picture
//         would be for a sound, and over a video that cannot play), look, expandable (the bar's
//         large-view button), autoPlay.
// Output: playing, position and duration (milliseconds), muted, volume (0 to 1), failed (the file could not be
//         played); pressed, activated and expandRequested.
// Keys (once it has the focus, which a press on it gives): Space plays and pauses, Left and Right
//         seek 5 seconds, Up and Down change the volume by 10%, M mutes.
// Cost:   a video shows its first frame by being paused at the start, so it holds a decoder from
//         the moment its source is set; a caller that does not want that leaves `source` empty
//         until the player is in view.
Item {
    id: view
    required property CanvasLook look
    property url source
    property string kind: "video"
    property string label
    property bool expandable: false
    property bool autoPlay: false
    property bool muted: false
    // 0 to 1; a change of it unmutes.
    property real volume: 1
    function setVolume(value) {
        view.volume = Math.max(0, Math.min(1, Math.round(value * 100) / 100))
        if (view.volume > 0)
            view.muted = false
    }

    readonly property bool playing: player.playbackState === MediaPlayer.PlayingState
    readonly property real position: player.position
    readonly property real duration: player.duration
    readonly property bool failed: player.error !== MediaPlayer.NoError
    readonly property bool hasSource: source.toString() !== ""
    readonly property alias engine: player
    readonly property real barHeight: 7 * look.unit // 28px: above the 24px floor
    signal pressed
    signal activated
    signal expandRequested

    function play() { if (hasSource) player.play() }
    function pause() { player.pause() }
    function toggle() { playing ? pause() : play() }
    function seek(milliseconds) { player.position = Math.max(0, Math.min(Math.max(0, duration), milliseconds)) }

    // m:ss, as the node's label writes a length.
    function clock(milliseconds) {
        const seconds = Math.max(0, Math.round(milliseconds / 1000))
        return Math.floor(seconds / 60) + ":" + String(seconds % 60).padStart(2, "0")
    }

    activeFocusOnTab: hasSource
    Accessible.role: Accessible.Grouping
    Accessible.name: label
    Keys.onSpacePressed: view.toggle()
    Keys.onLeftPressed: view.seek(player.position - 5000)
    Keys.onRightPressed: view.seek(player.position + 5000)
    Keys.onUpPressed: view.setVolume(view.volume + 0.1)
    Keys.onDownPressed: view.setVolume(view.volume - 0.1)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_M && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
            view.muted = !view.muted
            event.accepted = true
        }
    }

    MediaPlayer {
        id: player
        source: view.source
        audioOutput: AudioOutput { muted: view.muted; volume: view.volume }
        videoOutput: videoLoader.item
        // A video shows its first frame once loaded, by pausing at the start.
        onMediaStatusChanged: {
            if (mediaStatus === MediaPlayer.LoadedMedia) {
                if (view.autoPlay)
                    play()
                else if (view.kind === "video")
                    pause()
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: view.look.thumbCorner
        color: view.look.well
    }
    // The picture of a video: made only for a video, so a sound costs no scene graph item for it.
    Loader {
        id: videoLoader
        anchors.fill: parent
        active: view.kind === "video" && view.hasSource
        sourceComponent: VideoOutput {
            fillMode: VideoOutput.PreserveAspectFit
        }
    }
    Text {
        // The name, where there is no picture to play: a sound, or a video that cannot play.
        visible: view.kind !== "video" || view.failed || !view.hasSource
        anchors.fill: parent
        anchors.margins: 2 * view.look.unit
        anchors.bottomMargin: view.barHeight + 2 * view.look.unit
        text: view.failed ? qsTr("Cannot play this file") : view.label
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideMiddle
        wrapMode: Text.Wrap
        maximumLineCount: 2
        color: view.failed ? view.look.error : view.look.label
        font.pixelSize: view.look.labelSize
    }
    MouseArea {
        id: picture
        objectName: "mediaPicture"
        anchors.fill: parent
        onPressed: { view.forceActiveFocus(); view.pressed() }
        onDoubleClicked: view.activated()
    }
    Rectangle {
        anchors.fill: parent
        radius: view.look.thumbCorner
        color: "transparent"
        border.width: 1
        border.color: view.look.light ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(1, 1, 1, 0.1)
    }

    // The bar sits on the picture, not on the theme: white on black, as the thumbnail's chip.
    Rectangle {
        id: bar
        objectName: "mediaBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: view.barHeight
        radius: view.look.thumbCorner
        color: Qt.rgba(0, 0, 0, 0.6)

        component BarButton: Item {
            id: button
            property string glyph
            property string tip
            property bool on: false
            signal tapped
            width: view.barHeight
            height: view.barHeight
            opacity: enabled ? 1 : 0.4
            Accessible.role: Accessible.Button
            Accessible.name: tip
            Text {
                anchors.centerIn: parent
                text: view.look.glyph(button.glyph)
                font.family: view.look.iconFont
                font.pixelSize: 14
                color: "white"
            }
            Rectangle {
                anchors.fill: parent
                radius: view.look.smallCorner
                color: "transparent"
                border.width: button.activeFocus ? 2 : 0
                border.color: view.look.accent
            }
            activeFocusOnTab: enabled
            Keys.onReturnPressed: button.tapped()
            Keys.onSpacePressed: button.tapped()
            TapHandler { onTapped: { button.forceActiveFocus(); button.tapped() } }
            HoverHandler { cursorShape: button.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
            // A disabled item gets no hover (#281): while off, a handler on the parent stands in, in its coordinates.
            readonly property bool tipOver: enabled ? hoverTip.hovered
                : offHover.hovered && offHover.point.position.x >= x && offHover.point.position.x < x + width
                  && offHover.point.position.y >= y && offHover.point.position.y < y + height
            ToolTip.visible: tipOver && button.tip !== ""
            ToolTip.delay: App.theme.look.tooltipDelay
            ToolTip.text: button.tip
            HoverHandler { id: hoverTip }
            HoverHandler { id: offHover; parent: button.parent; enabled: !button.enabled; blocking: false }
        }

        BarButton {
            id: playButton
            objectName: "mediaPlay"
            anchors.left: parent.left
            enabled: view.hasSource && !view.failed
            glyph: view.playing ? "pause" : "play"
            tip: view.playing ? qsTr("Pause") : qsTr("Play")
            onTapped: view.toggle()
        }
        Text {
            id: time
            objectName: "mediaTime"
            anchors.left: playButton.right
            anchors.verticalCenter: parent.verticalCenter
            text: view.clock(seekTrack.scrubbing ? seekTrack.scrubAt : view.position) + " / " + view.clock(view.duration)
            color: "white"
            font.pixelSize: view.look.captionSize
            font.features: { "tnum": 1 }
        }
        Item {
            id: seekTrack
            objectName: "mediaSeek"
            readonly property bool scrubbing: seekPointer.pressed
            readonly property real scrubAt: view.duration > 0 ? Math.max(0, Math.min(1, seekPointer.mouseX / width)) * view.duration : 0
            anchors.left: time.right
            anchors.leftMargin: view.look.unit
            anchors.right: volumeTrack.left
            anchors.rightMargin: view.look.unit
            height: parent.height
            visible: width >= 3 * view.look.unit
            enabled: view.hasSource && view.duration > 0
            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Position")
            Rectangle {
                id: rail
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 4
                radius: 2
                color: Qt.rgba(1, 1, 1, 0.3)
                Rectangle {
                    width: view.duration > 0 ? parent.width * Math.max(0, Math.min(1, (seekTrack.scrubbing ? seekTrack.scrubAt : view.position) / view.duration)) : 0
                    height: parent.height
                    radius: 2
                    color: view.look.accent
                }
            }
            MouseArea {
                id: seekPointer
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onPressed: view.forceActiveFocus()
                onReleased: view.seek(seekTrack.scrubAt)
            }
        }
        Item {
            id: volumeTrack
            objectName: "mediaVolume"
            readonly property real shown: view.muted ? 0 : view.volume
            anchors.right: muteButton.left
            width: visible ? 14 * view.look.unit : 0
            height: parent.height
            // Only where there is room: a narrow node face keeps the mute button, and the wheel and
            // Up and Down still change the volume.
            visible: bar.width >= 70 * view.look.unit
            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Volume")
            Accessible.description: qsTr("%1%").arg(Math.round(shown * 100))
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 4
                radius: 2
                color: Qt.rgba(1, 1, 1, 0.3)
                Rectangle {
                    width: parent.width * volumeTrack.shown
                    height: parent.height
                    radius: 2
                    color: view.look.accent
                }
            }
            MouseArea {
                id: volumePointer
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onPressed: mouse => { view.forceActiveFocus(); view.setVolume(mouse.x / width) }
                onPositionChanged: mouse => { if (pressed) view.setVolume(mouse.x / width) }
                onWheel: wheel => view.setVolume(view.volume + (wheel.angleDelta.y > 0 ? 0.05 : -0.05))
            }
        }
        BarButton {
            id: muteButton
            objectName: "mediaMute"
            anchors.right: expandButton.left
            glyph: view.muted ? "volume-x" : "volume-2"
            tip: view.muted ? qsTr("Unmute") : qsTr("Mute (%1%; the wheel changes the volume)").arg(Math.round(view.volume * 100))
            onTapped: view.muted = !view.muted
            WheelHandler { onWheel: event => view.setVolume(view.volume + (event.angleDelta.y > 0 ? 0.05 : -0.05)) }
        }
        BarButton {
            id: expandButton
            objectName: "mediaExpand"
            anchors.right: parent.right
            width: visible ? view.barHeight : 0
            visible: view.expandable
            glyph: "maximize-2"
            tip: qsTr("Open large")
            onTapped: view.expandRequested()
        }
    }
}
