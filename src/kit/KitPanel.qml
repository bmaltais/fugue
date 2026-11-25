import QtQuick
import QtQuick.Shapes
import Fugue

// The card of one space of the App view (docs/canvas-look.md 20.4): the panel, the stage, the rail. A `kit-panel` base with a calm
// tint of its tone: three radial gradients of the tone's blobs fading to transparent at 70% (`panelTint` of the look's parameters,
// `tintCalm` of it for a `calm` card: the stage and the rail), a 1px edge, the card's corner. The tone reaches the controls inside
// through `kitTone` (KitTone.js). With Colour at 0 the tint is gone and the card is the flat fill. It takes children as any item.
Rectangle {
    id: panel
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property string tone: "dusk"
    property bool calm: false // the stage and the rail carry less colour than the panel
    readonly property string kitTone: tone
    readonly property real k: look.params.panelTint * (calm ? look.params.tintCalm : 1)
    readonly property var blobs: look.tone(tone).blobs

    readonly property bool panes: look.params.areaLayout === "panes" // edge to edge with 1px dividers, no corners
    radius: panes ? 0 : Math.min(look.params.cornerCard * look.unit, 0.2 * Math.min(width, height))
    color: look.popup
    border.width: panes || look.params.borders === "hairline" ? 1 : 0
    border.color: look.light ? Qt.rgba(0, 0, 0, 0.08) : Qt.rgba(1, 1, 1, 0.07)

    Shape {
        visible: panel.k > 0.001 && GraphicsInfo.api !== GraphicsInfo.Software
        anchors { fill: parent; margins: 1 }
        z: -1
        // The board's three radial gradients: (15%, 0), (90%, 30%) and (60%, 100%), about 90%, 75% and 95% of the width across.
        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: 0.15 * panel.width; centerY: 0 * panel.height
                centerRadius: 0.9 * panel.width
                focalX: centerX; focalY: centerY
                GradientStop { position: 0; color: Qt.alpha(panel.blobs[0], panel.k) }
                GradientStop { position: 0.7; color: Qt.alpha(panel.blobs[0], 0) }
            }
            PathRectangle { x: 0; y: 0; width: panel.width - 2; height: panel.height - 2; radius: Math.max(0, panel.radius - 1) }
        }
        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: 0.9 * panel.width; centerY: 0.3 * panel.height
                centerRadius: 0.75 * panel.width
                focalX: centerX; focalY: centerY
                GradientStop { position: 0; color: Qt.alpha(panel.blobs[1], panel.k * panel.look.params.tintBlob1) }
                GradientStop { position: 0.7; color: Qt.alpha(panel.blobs[1], 0) }
            }
            PathRectangle { x: 0; y: 0; width: panel.width - 2; height: panel.height - 2; radius: Math.max(0, panel.radius - 1) }
        }
        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: 0.6 * panel.width; centerY: 1 * panel.height
                centerRadius: 0.95 * panel.width
                focalX: centerX; focalY: centerY
                GradientStop { position: 0; color: Qt.alpha(panel.blobs[2], panel.k * panel.look.params.tintBlob2) }
                GradientStop { position: 0.7; color: Qt.alpha(panel.blobs[2], 0) }
            }
            PathRectangle { x: 0; y: 0; width: panel.width - 2; height: panel.height - 2; radius: Math.max(0, panel.radius - 1) }
        }
    }
}
