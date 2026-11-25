import QtQuick
import QtQuick.Shapes
import Fugue

// The local colour halo of a coloured control (docs/canvas-look.md 20.11): a soft elliptical radial gradient of the tone behind an
// "on" switch or along a slider's filled part, faint ("a hint of light, never a glow effect"), faded in and out with `lit` over
// `durHalo`. It is a child of the control, behind it, centred on `centerX`/`centerY` in the control's coordinates, `boxWidth` by
// `boxHeight` px; the panel above clips it. Not drawn with Frost at 0. It takes no input.
Item {
    id: halo
    property CanvasLook look: App.theme.look
    property color tone: look.accent
    property bool lit: false
    property real centerX: 0
    property real centerY: 0
    property real boxWidth: look.params.haloWidth * look.unit
    property real boxHeight: look.params.haloHeight * look.unit
    enabled: false
    visible: look.frost > 0.001 && opacity > 0.001 && GraphicsInfo.api !== GraphicsInfo.Software
    opacity: lit ? look.params.haloOpacity : 0
    z: -1
    Behavior on opacity { NumberAnimation { duration: halo.look.params.durHalo; easing.type: Easing.InOutQuad } }
    Shape {
        x: halo.centerX - halo.boxWidth / 2
        y: halo.centerY - halo.boxWidth / 2
        width: halo.boxWidth
        height: halo.boxWidth
        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: halo.boxWidth / 2; centerY: halo.boxWidth / 2
                centerRadius: halo.boxWidth / 2
                focalX: centerX; focalY: centerY
                GradientStop { position: 0; color: Qt.alpha(halo.tone, halo.look.params.haloStop) }
                GradientStop { position: 1; color: Qt.alpha(halo.tone, 0) }
            }
            startX: 0; startY: 0
            PathLine { x: halo.boxWidth; y: 0 }
            PathLine { x: halo.boxWidth; y: halo.boxWidth }
            PathLine { x: 0; y: halo.boxWidth }
        }
        // A circle of radius boxWidth/2 stretched to the box's height: the ellipse of the board's closest-side gradient.
        transform: Scale { origin.x: halo.boxWidth / 2; origin.y: halo.boxWidth / 2; yScale: halo.boxWidth > 0 ? halo.boxHeight / halo.boxWidth : 1 }
    }
}
