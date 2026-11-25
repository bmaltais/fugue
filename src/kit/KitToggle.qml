import QtQuick
import Fugue
import "KitTone.js" as KitTone

// A pill switch of the Drift kit (docs/canvas-look.md 20.8): recessed glass, in the window's tone as translucent colour when on,
// with a raised translucent white knob, and a faint halo of the tone behind it when on (KitHalo). The track is 11u by 6.5u in an
// 11u by 7u press area (the 7u floor, docs/ui-ergonomics.md). The tone comes from the KitPanel above (`tone`); outside a panel the
// accent. With Frost at 0 it is opaque: the plain rail when off, the tone mixed into the group when on, an opaque knob, no halo.
// The value comes in through `checked` and the change goes out through `toggled(value)`: the switch never sets `checked` itself.
// Space flips it, Tab reaches it; assistive tools hear a switch named by `text`. Put it in a KitRow's trailing slot and point the
// row's `target` at it, and a click anywhere on the row flips it too.
Item {
    id: control
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property bool checked: false
    property string text // the name assistive tools say; a KitRow with a label names its own switch
    property string tone: KitTone.nearest(control) // the panel's tone; "" is the accent
    signal toggled(bool value)

    readonly property color toneColor: tone === "" || look.params.controlColour === "accent" ? look.accent : look.tone(tone).glass
    readonly property bool glass: look.frost > 0.001
    readonly property var pm: look.params

    // What a row's click calls: asks for the opposite value.
    function toggle() {
        if (enabled)
            toggled(!checked)
    }

    implicitWidth: 11 * look.unit
    implicitHeight: 7 * look.unit
    width: implicitWidth
    height: implicitHeight
    activeFocusOnTab: enabled
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.CheckBox
    Accessible.name: text
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.onPressAction: control.toggle()
    Accessible.onToggleAction: control.toggle()

    KitHalo {
        look: control.look
        tone: control.toneColor
        lit: control.checked && control.glass
        centerX: control.width - 15 * control.look.unit
        centerY: control.height / 2
    }
    Rectangle {
        id: track
        anchors.centerIn: parent
        width: 11 * control.look.unit
        height: 6.5 * control.look.unit
        radius: height / 2
        gradient: Gradient {
            GradientStop { position: 0; color: control.checked ? (control.glass ? Qt.alpha(control.toneColor, control.pm.toggleOnTop) : Qt.tint(control.look.kitGroup, Qt.alpha(control.toneColor, 0.75)))
                                                         : (control.glass ? Qt.rgba(0, 0, 0, control.look.light ? control.pm.glassTrackAlphaLight : control.pm.glassTrackAlpha) : control.look.kitTrack) }
            GradientStop { position: 1; color: control.checked ? (control.glass ? Qt.alpha(control.toneColor, control.pm.toggleOnBottom) : Qt.tint(control.look.kitGroup, Qt.alpha(control.toneColor, 0.75)))
                                                         : (control.glass ? Qt.rgba(0, 0, 0, control.look.light ? control.pm.glassTrackAlphaLight : control.pm.glassTrackAlpha) : control.look.kitTrack) }
        }
        border.width: control.activeFocus && FocusInput.keyboard ? 2 : control.glass ? 1 : 0
        border.color: control.activeFocus && FocusInput.keyboard ? control.look.accent : Qt.rgba(0, 0, 0, control.pm.trackEdge * (control.look.light ? 0.64 : 1))
        Rectangle { // the inner shadow along the top edge
            visible: control.glass
            x: track.radius * 0.6
            y: 1
            width: parent.width - 2 * x
            height: Math.max(1, control.pm.wellShadowHeight)
            color: Qt.rgba(0, 0, 0, control.look.light ? control.pm.wellShadowAlphaLight : control.pm.glassTrackShadow * control.pm.wellShadowAlpha / 0.22)
        }
        Rectangle {
            id: knob
            readonly property real gap: 0.5 * control.look.unit
            width: track.height - 2 * gap
            height: width
            radius: width / 2
            anchors.verticalCenter: parent.verticalCenter
            x: control.checked ? track.width - width - gap : gap
            Behavior on x { NumberAnimation { duration: control.pm.durToggle; easing.type: Easing.InOutQuad } }
            Rectangle { // the knob's drop shadow
                anchors { fill: parent; topMargin: control.pm.raiseShadowOffset; bottomMargin: -control.pm.raiseShadowOffset }
                z: -1
                radius: width / 2
                color: Qt.rgba(0, 0, 0, control.look.light ? 0.25 : 0.38)
                visible: control.pm.raiseShadowOffset > 0.01
            }
            gradient: Gradient {
                GradientStop { position: 0; color: control.glass ? Qt.rgba(1, 1, 1, control.look.light ? 0.97 : control.pm.knobTop) : control.look.kitKnob }
                GradientStop { position: 1; color: control.glass ? Qt.rgba(1, 1, 1, control.look.light ? 0.88 : control.pm.knobBottom) : control.look.kitKnob }
            }
            border.width: control.glass ? 1 : 0
            border.color: control.look.light ? Qt.rgba(0, 0, 0, 0.14) : Qt.rgba(1, 1, 1, control.pm.knobEdge)
            Rectangle { // the light along its top edge
                visible: control.glass
                x: parent.radius * 0.5
                y: 1
                width: parent.width - 2 * x
                height: 1
                color: Qt.rgba(1, 1, 1, control.pm.knobShine)
            }
        }
    }
    // The press is the switch's own, so the row it sits in does not also flip it.
    TapHandler {
        gesturePolicy: TapHandler.WithinBounds
        onTapped: control.toggle()
    }
    HoverHandler { cursorShape: control.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
    Keys.onSpacePressed: control.toggle()
}
