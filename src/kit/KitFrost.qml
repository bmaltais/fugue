import QtQuick
import Fugue

// The frosted glass of the Drift kit's controls over a picture (docs/canvas-look.md "Drift kit"), smart about contrast: the
// control measures how light the picture under it is (`PictureLuma`, a small downscaled grab of the `backdrop`, taken once per
// picture and position, not per frame) and decides for itself, so a mixed picture gives each control its own answer.
//
//   dark picture   white ink on a light tint over a dark base (the base darkens until white reads at 4.5:1), a light 1px edge
//   pale picture   dark ink on a whiter tint (white 30 to 60% by the frost setting, raised until the dark ink reads at 4.5:1),
//                  a faint dark 1px edge
//
// The picture behind the control is blurred when the renderer can (a small `ShaderEffectSource` of the rectangle behind the
// control, scaled up smoothly: a soft blur with no shader of its own, cut to the rounded shape by the thumbnail shader); the
// software renderer has no source textures, so there the tint and base stand alone. Everything scales with `look.frost`
// (settings.json `frost`, 0 to 1, 0.6 by default): tint, edge, shadow and blur. At 0 the control is `plainColor`, opaque, with
// no glass and no measuring. The control reads `ink` and `pale`. Put it first in the control, filling it; it takes no input.
Item {
    id: frost
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property Item backdrop: null
    // Smoked glass, for a field over a picture (KitWell with a backdrop): a dark translucent tint (black 35 to 55% over a dark picture,
    // 55 to 75% over a pale one, raised until white text reads at 4.5:1), the blur, a 1px shadow along the top and a faint light
    // line along the bottom; no white tint. `labelInk` is the ink of a label beside it: dark over a pale picture.
    property bool smoked: false
    // The picture's url when the backdrop is one: a change measures again.
    property url backdropSource
    property real radius: height / 2
    property bool hovered: false
    property bool pressed: false
    // A toggle that is on (KitChip.current): lit glass, no accent. Over a dark picture a near-white tint with dark ink and a bright edge; over a pale
    // one the inverse, a dark base (raised until white ink reads at 4.5:1) with white ink. Frost 0 is the plain control, which the chip fills itself.
    property bool on: false
    readonly property bool lit: on && !frozen && !smoked
    property color plainColor: look.kitHeroPill // what the control is with the frost off
    readonly property bool frozen: look.frost <= 0.001
    readonly property real strength: look.frost / 0.6 // 1 at the default
    readonly property bool blurred: !frozen && backdrop !== null && GraphicsInfo.api !== GraphicsInfo.Software

    // The mean relative luminance under the control, -1 until measured (a dark picture is assumed meanwhile).
    property real patch: -1
    readonly property bool pale: !frozen && patch > PictureLuma.paleThreshold()
    readonly property color darkInk: look.light ? look.text : "#17171b"
    readonly property color ink: frozen || smoked ? "white" : (pale !== lit) ? darkInk : "white"
    readonly property color labelInk: frozen ? look.text : pale ? darkInk : "white"
    // The dark base alpha under the white ink of a lit chip over a pale picture.
    readonly property real litBase: {
        const lum = patch < 0 ? 0.2 : patch
        let b = 0.62
        while (b < 0.95 && 1.05 / (lum * (1 - b) + 0.05) < 4.5)
            b += 0.05
        return b
    }
    // The tint alpha and the dark base alpha that make the ink read at 4.5:1 over the measured patch.
    readonly property var plan: {
        const lum = patch < 0 ? 0.2 : patch
        const contrast = (a, b) => (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05)
        if (smoked) {
            let dark = pale ? 0.55 + 0.20 * Math.min(1, strength) : 0.35 + 0.20 * Math.min(1, strength)
            while (dark < 0.9 && contrast(1, lum * (1 - dark)) < 4.5)
                dark += 0.05
            return {tint: 0, base: dark}
        }
        if (pale) {
            let tint = Math.min(0.6, 0.30 + 0.30 * Math.min(1, strength))
            const inkLum = PictureLuma.relativeLuminance(darkInk)
            while (tint < 0.9 && contrast(lum * (1 - tint) + tint, inkLum) < 4.5)
                tint += 0.05
            return {tint: tint, base: 0}
        }
        // The base and the tint are blended on the gamma-encoded colour, as the scene graph does: the luminance under white ink is
        // the picture darkened by `base`, then lightened by `tint`, both mixed in sRGB-ish space (power 2.2), not in linear light.
        const tint = Math.min(0.5, look.kitFrost.a * strength)
        const under = base => Math.pow(Math.pow(lum, 1 / 2.2) * (1 - base) * (1 - tint) + tint, 2.2)
        let base = blurred ? 0.22 : 0.40
        while (base < 0.8 && contrast(1, under(base)) < 4.5)
            base += 0.05
        return {tint: tint, base: base}
    }

    anchors.fill: parent
    enabled: false // no input of its own

    // Measuring: a grab of the backdrop at 64 px, the mean taken over the rectangle behind this control. Where the renderer has
    // source textures the grab is taken from a hidden probe of the backdrop (the hero hides its own picture under a shader).
    // The window is asked through the attached `Window.window`: an Item has no `window` property in QML, so `backdrop.window` was
    // always undefined, the measuring never ran and every glass stayed at its unmeasured, nearly black base.
    function measure() {
        if (frozen || !backdrop || !backdrop.Window.window || backdrop.width <= 0 || backdrop.height <= 0 || width <= 0)
            return
        const at = frost.mapToItem(backdrop, 0, 0, width, height)
        const nx = at.x / backdrop.width, ny = at.y / backdrop.height, nw = at.width / backdrop.width, nh = at.height / backdrop.height
        const target = blurred && probeLoader.item ? probeLoader.item : backdrop
        target.grabToImage(result => {
            const lum = PictureLuma.meanOfImage(result.image, nx, ny, nw, nh)
            if (lum >= 0)
                frost.patch = lum
        }, Qt.size(64, Math.max(1, Math.round(64 * backdrop.height / backdrop.width))))
    }
    Timer { id: later; interval: 150; onTriggered: frost.measure() }
    Component.onCompleted: later.restart()
    onBackdropChanged: later.restart()
    onBackdropSourceChanged: later.restart()
    onWidthChanged: later.restart()
    onHeightChanged: later.restart()
    onXChanged: later.restart()
    onYChanged: later.restart()
    onFrozenChanged: later.restart()
    Connections {
        target: frost.backdrop
        function onWidthChanged() { later.restart() }
        function onHeightChanged() { later.restart() }
    }

    Rectangle {
        id: shape
        anchors.fill: parent
        radius: frost.radius
        color: "black"
        visible: false
    }
    // A clipped nothing that holds the probe: it is never seen, only grabbed.
    Item {
        width: 0
        height: 0
        clip: true
        Loader {
            id: probeLoader
            active: frost.blurred
            sourceComponent: ShaderEffectSource {
                width: frost.backdrop ? frost.backdrop.width : 0
                height: frost.backdrop ? frost.backdrop.height : 0
                sourceItem: frost.backdrop
                live: true
            }
        }
    }
    // The blur: the rectangle behind the control as a texture a fraction of its size, scaled up smoothly.
    Loader {
        anchors.fill: parent
        active: frost.blurred
        sourceComponent: ShaderEffect {
            readonly property variant source: snap
            readonly property size size: Qt.size(width, height)
            readonly property real radius: frost.radius
            fragmentShader: "qrc:/canvas/shaders/thumbnail.frag.qsb"
            ShaderEffectSource {
                id: snap
                visible: false
                live: true
                smooth: true
                sourceItem: frost.backdrop
                textureSize: Qt.size(Math.max(2, Math.round(frost.width / (3 + 6 * frost.strength))),
                                     Math.max(2, Math.round(frost.height / (3 + 6 * frost.strength))))
                sourceRect: frost.mapToItem(frost.backdrop, 0, 0, frost.width, frost.height)
            }
        }
    }
    Rectangle { // the soft shadow under the glass
        anchors { fill: parent; topMargin: 1; bottomMargin: -1 }
        visible: !frost.frozen
        radius: frost.radius
        color: Qt.rgba(0, 0, 0, Math.min(0.35, 0.22 * frost.strength))
        z: -1
    }
    Rectangle { // the plain control, with the frost off
        anchors.fill: parent
        visible: frost.frozen
        radius: frost.radius
        color: frost.pressed || frost.hovered ? Qt.darker(frost.plainColor, 1.3) : frost.plainColor
    }
    Rectangle { // the dark base that keeps white ink at 4.5:1 over a dark or mid picture
        anchors.fill: parent
        visible: !frost.frozen && frost.plan.base > 0
        radius: frost.radius
        color: Qt.rgba(0, 0, 0, frost.plan.base)
    }
    Rectangle { // the tint: a little brighter at the top, so it reads over a flat colour too
        anchors.fill: parent
        visible: !frost.frozen && !frost.smoked
        radius: frost.radius
        readonly property real lift: frost.hovered || frost.pressed ? 0.1 : 0
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.rgba(1, 1, 1, Math.min(0.95, frost.plan.tint * (frost.pale ? 1.1 : 1.45) + parent.lift)) }
            GradientStop { position: 1; color: Qt.rgba(1, 1, 1, Math.min(0.95, frost.plan.tint * (frost.pale ? 0.9 : 0.8) + parent.lift)) }
        }
        border.width: 1
        border.color: frost.pale ? Qt.rgba(0, 0, 0, 0.18 * Math.min(1.5, frost.strength))
                                 : Qt.alpha(frost.look.kitFrostEdge, Math.min(0.7, frost.look.kitFrostEdge.a * frost.strength))
    }
    Rectangle { // smoked glass: the 1px shadow along the top and the light line along the bottom
        visible: !frost.frozen && frost.smoked
        x: frost.radius * 0.6
        width: parent.width - 2 * x
        height: 1
        y: 1
        color: Qt.rgba(0, 0, 0, 0.35)
    }
    Rectangle {
        visible: !frost.frozen && frost.smoked
        x: frost.radius * 0.6
        width: parent.width - 2 * x
        height: 1
        y: parent.height - 2
        color: Qt.rgba(1, 1, 1, 0.12)
    }
    Rectangle { // on: lit glass over the tint, faded in over durColor
        objectName: "frostOn"
        anchors.fill: parent
        radius: frost.radius
        opacity: frost.lit ? 1 : 0
        visible: opacity > 0
        color: frost.pale ? Qt.rgba(0, 0, 0, frost.litBase) : Qt.rgba(1, 1, 1, Math.min(0.95, 0.78 + (frost.hovered || frost.pressed ? 0.08 : 0)))
        border.width: frost.pale ? 0 : 1
        border.color: Qt.rgba(1, 1, 1, 0.9)
        Behavior on opacity { NumberAnimation { duration: frost.look.params.durColor } }
    }
}
