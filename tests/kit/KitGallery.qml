import QtQuick
import Fugue
import "qrc:/kit"

// Every component of the Drift kit in one card, laid out as omarchy-drift's settings card is (docs/canvas-look.md
// "Drift kit"). The test grabs it in a dark and a light theme; it is also the page to open when the kit changes.
Item {
    id: gallery
    property url heroSource
    property int cardWidth: 380
    property real heroMin: 28 // in u; 0 is the compact header of an app panel
    property string heroTone: "dusk" // KitHero.tone: the blend that says which part of Fugue this is
    property bool following: true
    property bool wander: false
    property bool followDay: true
    property bool resting: false
    property real speed: 1.2
    property real grain: 0.4
    property real blobs: 6
    property bool paused: false
    width: cardWidth + 40
    height: card.height + 40

    Rectangle {
        anchors.fill: parent
        color: App.theme.look.canvas
    }
    Rectangle {
        id: card
        x: 20
        y: 20
        width: gallery.cardWidth
        height: content.height
        radius: App.theme.look.popupCorner + 6
        color: App.theme.look.popup
        border.width: 1
        border.color: App.theme.look.hairline
        Column {
            id: content
            width: parent.width
            KitHero {
                objectName: "hero"
                source: gallery.heroSource
                tone: gallery.heroTone
                title: "Drift"
                subtitle: "A quiet world, always becoming."
                status: gallery.paused ? "Paused" : "Evolving"
                showClose: true
                cornerRadius: card.radius
                minHeight: gallery.heroMin * App.theme.look.unit
            }
            Column {
                x: 3.5 * App.theme.look.unit
                width: parent.width - 2 * x
                bottomPadding: 4.5 * App.theme.look.unit
                KitSection { text: "Colour"; topPadding: 4 * App.theme.look.unit }
                KitGroup {
                    KitRow {
                        label: "Atmosphere"
                        KitChip { text: "Weather"; glyph: "chevrons-up-down"; tip: "Choose an atmosphere" }
                        KitChip { text: "Generate"; glyph: "sparkles" }
                    }
                    KitRow {
                        label: "Wander through palettes"
                        target: wanderToggle
                        KitToggle { id: wanderToggle; checked: gallery.wander; onToggled: v => gallery.wander = v }
                    }
                    KitRow {
                        label: "Follow the day"
                        hint: "Local light, slowly shifting"
                        target: dayToggle
                        KitToggle { id: dayToggle; checked: gallery.followDay; onToggled: v => gallery.followDay = v }
                    }
                    KitRow {
                        label: "Follow the weather"
                        hint: "Rain, snow and cloud on the glass"
                        target: weatherToggle
                        KitChip { visible: gallery.following; text: "Gatineau"; glyph: "navigation" }
                        KitToggle { id: weatherToggle; checked: gallery.following; onToggled: v => gallery.following = v }
                    }
                }
                KitSection { text: "Motion" }
                KitGroup {
                    KitRow {
                        label: "Speed"
                        KitSlider {
                            objectName: "speed"
                            from: 0.05; to: 3; value: gallery.speed
                            startGlyph: "turtle"; endGlyph: "rabbit"
                            onMoved: v => gallery.speed = v
                        }
                    }
                    KitRow {
                        label: "Rest & drift"
                        hint: "Hold still, then gently drift for 30 seconds"
                        target: restToggle
                        KitToggle { id: restToggle; checked: gallery.resting; onToggled: v => gallery.resting = v }
                    }
                }
                KitSection { text: "Shape" }
                KitGroup {
                    KitRow {
                        label: "Grain"
                        KitSlider { from: 0; to: 1; value: gallery.grain; onMoved: v => gallery.grain = v }
                    }
                    KitRow {
                        label: "Blobs"
                        KitSlider {
                            from: 2; to: 8; stepSize: 1; ticks: true; showValue: true; value: gallery.blobs
                            onMoved: v => gallery.blobs = v
                        }
                    }
                    Item { // a picture with a field over it: smoked glass
                        width: parent.width
                        height: 11 * App.theme.look.unit
                        Rectangle {
                            id: pictureBehind
                            anchors { fill: parent; topMargin: 2 * App.theme.look.unit; bottomMargin: 2 * App.theme.look.unit }
                            radius: App.theme.look.popupCorner
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0; color: "#2a2f4a" }
                                GradientStop { position: 0.5; color: "#c46b8a" }
                                GradientStop { position: 1; color: "#f2eadb" }
                            }
                        }
                        Rectangle {
                            x: 3 * App.theme.look.unit
                            anchors.verticalCenter: parent.verticalCenter
                            width: 18 * App.theme.look.unit
                            height: 7 * App.theme.look.unit
                            color: "transparent"
                            KitWell { id: smoked; backdrop: pictureBehind }
                            Text { x: 2 * App.theme.look.unit; anchors.verticalCenter: parent.verticalCenter; text: "Gatineau"; color: smoked.ink; font.pixelSize: App.theme.look.textSize }
                        }
                        Rectangle {
                            anchors { right: parent.right; rightMargin: 3 * App.theme.look.unit; verticalCenter: parent.verticalCenter }
                            width: 18 * App.theme.look.unit
                            height: 7 * App.theme.look.unit
                            color: "transparent"
                            KitWell { id: smokedPale; backdrop: pictureBehind }
                            Text { x: 2 * App.theme.look.unit; anchors.verticalCenter: parent.verticalCenter; text: "Over pale"; color: smokedPale.ink; font.pixelSize: App.theme.look.textSize }
                        }
                    }
                    KitRow {
                        label: "Name"
                        hint: "A field is set in; a chip is raised"
                        Rectangle {
                            width: 30 * App.theme.look.unit
                            height: 7 * App.theme.look.unit
                            color: "transparent"
                            KitWell { }
                            Text {
                                x: 2 * App.theme.look.unit
                                anchors.verticalCenter: parent.verticalCenter
                                text: "Gatineau"
                                color: App.theme.look.text
                                font.pixelSize: App.theme.look.textSize
                            }
                        }
                    }
                }
            }
            KitSeparator { }
            Row {
                x: 4.5 * App.theme.look.unit
                height: 13 * App.theme.look.unit
                spacing: 2 * App.theme.look.unit
                KitChip {
                    anchors.verticalCenter: parent.verticalCenter
                    pill: true; glyph: gallery.paused ? "play" : "pause"; tip: gallery.paused ? "Resume" : "Pause"
                    onTapped: gallery.paused = !gallery.paused
                }
                KitChip { anchors.verticalCenter: parent.verticalCenter; pill: true; glyph: "shuffle"; tip: "Shuffle" }
            }
        }
    }
}
