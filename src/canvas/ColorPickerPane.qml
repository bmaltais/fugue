pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue

// A color picker (#52, #122; docs/ui-ergonomics.md "Color picker") in the manner of Adobe's: a
// saturation and brightness field with a hue slider beside it, the new color over the current one,
// H S B and R G B number fields that follow each other, a hex field, the colors used lately, and
// Cancel and OK. It is self-contained: it knows nothing of nodes, Frames or the canvas, so any
// surface can use it (a node's color widget, a settings page, the canvas's layout colors).
//
//   color       (read) the color being chosen now, "#rrggbb"; follows every drag and keystroke.
//   setColor(c) starts the picker on `c` ("#rrggbb", "#rgb" or without the "#"); a bad value is
//               ignored. `c` is also the "current" swatch, which a click puts back.
//   picked(c)   the person confirmed `c`: OK, Enter in a field or on the field and slider, or a
//               click on a recent color. The color is also put first in the recent list
//               (App.colorHistory), unless `remember` is false. Nothing else changes: the caller
//               decides what picking does.
//   canceled()  Cancel, or Escape. The caller closes or ignores it.
//   remember    whether picked() adds the color to the recent list; true.
//   showRecent  whether the recent colors show; true.
//   focusField() puts the keyboard in the hex field.
//
// It sizes itself (implicitWidth, implicitHeight) and draws no surface of its own: ColorPickerPopup
// puts it on a popup. The recent list and the color math are App.colorHistory (menu/color_history.h,
// over engine/layout_color.h).
Item {
    id: root

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property ColorHistory history: App.colorHistory
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft

    property bool remember: true
    property bool showRecent: true
    signal picked(string color)
    signal canceled

    // The color being chosen: hue (degrees), saturation and value (0 to 1) are the truth, the
    // fields show them rounded. A gray keeps its hue, so the field does not jump when it is left.
    property real hue: 0
    property real sat: 1
    property real val: 1
    readonly property string color: history ? history.hex(hue, sat, val) : "#000000"
    // The color the picker started on: the "current" swatch.
    property string original: "#000000"

    readonly property var hsbShown: history ? history.hsb(hue, sat, val) : ({h: 0, s: 0, b: 0})
    readonly property var rgbShown: history ? history.rgb(color) : ({r: 0, g: 0, b: 0})
    readonly property bool hexValid: history && history.normalize(hexField.text) !== ""

    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight

    // The look of the sliders' and swatches' hairlines on either theme.
    readonly property color edge: look.hairlineStrong
    readonly property color hueColor: history ? history.hex(hue, 1, 1) : "red"

    function clamp01(x) { return Math.max(0, Math.min(1, x)) }

    function setColor(color) {
        const normal = history ? history.normalize(color) : ""
        if (normal === "")
            return
        original = normal
        const c = history.hsv(normal)
        hue = c.h
        sat = c.s
        val = c.v
        syncFields(true)
    }

    // Takes a hex color a field gave: hue stays where it was when the color has none (a gray).
    function chooseHex(hex) {
        const c = history.hsv(hex)
        if (c.s > 0 && c.v > 0)
            hue = c.h
        sat = c.s
        val = c.v
        syncFields(false)
    }

    // Every field shows the color, but the one being typed in.
    function syncFields(force) {
        if (force || !hexField.activeFocus)
            hexField.text = root.color
        for (const f of [fieldH, fieldS, fieldB, fieldR, fieldG, fieldBlue])
            f.sync(force)
    }

    function focusField() { hexField.forceActiveFocus() }

    function confirm(color) {
        const normal = history ? history.normalize(color) : ""
        if (normal === "")
            return
        if (remember)
            history.use(normal)
        picked(normal)
    }

    // OK and Enter: the color of the fields, when the hex field holds one.
    function ok() {
        if (hexValid)
            confirm(root.color)
    }

    // The hue, saturation and brightness moved by hand: the fields follow.
    function moved() { syncFields(false) }

    Keys.onEscapePressed: event => { event.accepted = true; root.canceled() }

    // A handle: a ring in white over a dark outer ring and a soft shadow, so it reads on any color.
    component Handle: Item {
        id: handle
        property real cornerRadius: height / 2
        property bool active: false // hovered or pressed: a touch larger
        property color fill: "transparent"
        scale: active ? 1.1 : 1
        Repeater { // the shadow: three offset layers, fainter outward
            model: [{grow: 2, alpha: 0.10}, {grow: 1, alpha: 0.16}, {grow: 0, alpha: 0.24}]
            delegate: Rectangle {
                required property var modelData
                x: -modelData.grow
                y: 1 - modelData.grow
                width: handle.width + 2 * modelData.grow
                height: handle.height + 2 * modelData.grow
                radius: handle.cornerRadius + modelData.grow
                color: Qt.rgba(0, 0, 0, modelData.alpha)
            }
        }
        Rectangle { // the color the handle holds, when it holds one
            anchors.fill: parent
            radius: handle.cornerRadius
            color: handle.fill
        }
        Rectangle { // the outer ring, dark
            anchors.fill: parent
            radius: handle.cornerRadius
            color: "transparent"
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, 0.55)
        }
        Rectangle { // the ring, white
            anchors.fill: parent
            anchors.margins: 1
            radius: Math.max(0, handle.cornerRadius - 1)
            color: "transparent"
            border.width: 2
            border.color: "white"
        }
        Rectangle { // the inner ring, dark
            anchors.fill: parent
            anchors.margins: 3
            radius: Math.max(0, handle.cornerRadius - 3)
            color: "transparent"
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, 0.4)
        }
    }

    // The well of a field or slider: a 1px edge, a darker line inside it on top as if sunk, and
    // the focus ring (the accent, 2px) outside it.
    component Well: Item {
        id: well
        property bool focused: false
        property real cornerRadius: root.look.smallCorner
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: well.cornerRadius + 2
            color: "transparent"
            border.width: 2
            border.color: root.look.accent
            visible: well.focused && FocusInput.keyboard
        }
        Rectangle {
            anchors.fill: parent
            radius: well.cornerRadius
            color: "transparent"
            border.width: 1
            border.color: root.edge
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: Math.max(0, well.cornerRadius - 1)
            color: "transparent"
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, root.look.light ? 0.10 : 0.30)
        }
    }

    ColumnLayout {
        id: layout
        anchors.fill: parent
        spacing: 3 * root.u // a chunk apart: twice the 1u inside a chunk, and more
        LayoutMirroring.enabled: root.rightToLeft
        LayoutMirroring.childrenInherit: true

        // The field, the hue slider, and at their side the swatches and the number fields.
        RowLayout {
            spacing: 3 * root.u

            RowLayout {
                Layout.alignment: Qt.AlignTop
                spacing: 2 * root.u

                // Saturation across, brightness down: the picked hue at the top right.
                Item {
                    id: field
                    objectName: "colorField"
                    implicitWidth: 48 * root.u
                    implicitHeight: 48 * root.u
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Slider
                    Accessible.name: qsTr("Saturation and brightness")
                    readonly property bool engaged: fieldMouse.pressed || fieldMouse.containsMouse

                    function pick(x, y) {
                        root.sat = root.clamp01(x / width)
                        root.val = root.clamp01(1 - y / height)
                        root.moved()
                    }
                    Keys.onPressed: event => {
                        const step = (event.modifiers & Qt.ShiftModifier ? 1 : 5) / 100
                        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                            root.sat = root.clamp01(root.sat + (event.key === Qt.Key_Right ? step : -step))
                        } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                            root.val = root.clamp01(root.val + (event.key === Qt.Key_Up ? step : -step))
                        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            root.ok()
                        } else {
                            return
                        }
                        root.moved()
                        event.accepted = true
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: root.look.smallCorner
                        color: root.hueColor
                    }
                    Rectangle { // toward the left it washes out to white: saturation
                        anchors.fill: parent
                        radius: root.look.smallCorner
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0; color: "#ffffffff" }
                            GradientStop { position: 1; color: "#00ffffff" }
                        }
                    }
                    Rectangle { // toward the bottom it goes to black: brightness
                        anchors.fill: parent
                        radius: root.look.smallCorner
                        gradient: Gradient {
                            GradientStop { position: 0; color: "#00000000" }
                            GradientStop { position: 1; color: "#ff000000" }
                        }
                    }
                    Well { anchors.fill: parent; focused: field.activeFocus }
                    Handle {
                        objectName: "colorFieldHandle"
                        x: root.sat * field.width - width / 2
                        y: (1 - root.val) * field.height - height / 2
                        width: 4 * root.u
                        height: width
                        active: field.engaged
                    }
                    MouseArea {
                        id: fieldMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.CrossCursor
                        onPressed: mouse => { field.forceActiveFocus(); field.pick(mouse.x, mouse.y) }
                        onPositionChanged: mouse => { if (pressed) field.pick(mouse.x, mouse.y) }
                    }
                }

                // Hue: red at the top, round through the colors and back to red at the bottom.
                Item {
                    id: slider
                    objectName: "colorHue"
                    implicitWidth: 6 * root.u
                    implicitHeight: field.implicitHeight
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Slider
                    Accessible.name: qsTr("Hue")
                    readonly property bool engaged: sliderMouse.pressed || sliderMouse.containsMouse

                    function pick(y) {
                        root.hue = Math.min(359.99, root.clamp01(y / height) * 360)
                        root.moved()
                    }
                    Keys.onPressed: event => {
                        const step = event.modifiers & Qt.ShiftModifier ? 1 : 5
                        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                            root.hue = (root.hue + (event.key === Qt.Key_Down ? step : -step) + 360) % 360
                        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            root.ok()
                        } else {
                            return
                        }
                        root.moved()
                        event.accepted = true
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: root.look.smallCorner
                        gradient: Gradient {
                            GradientStop { position: 0 / 6; color: "#ff0000" }
                            GradientStop { position: 1 / 6; color: "#ffff00" }
                            GradientStop { position: 2 / 6; color: "#00ff00" }
                            GradientStop { position: 3 / 6; color: "#00ffff" }
                            GradientStop { position: 4 / 6; color: "#0000ff" }
                            GradientStop { position: 5 / 6; color: "#ff00ff" }
                            GradientStop { position: 6 / 6; color: "#ff0000" }
                        }
                    }
                    Well { anchors.fill: parent; focused: slider.activeFocus }
                    Handle {
                        objectName: "colorHueHandle"
                        x: -2
                        y: root.hue / 360 * slider.height - height / 2
                        width: slider.width + 4
                        height: 3 * root.u
                        cornerRadius: root.look.smallCorner
                        fill: root.hueColor
                        active: slider.engaged
                    }
                    MouseArea {
                        id: sliderMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.SizeVerCursor
                        onPressed: mouse => { slider.forceActiveFocus(); slider.pick(mouse.y) }
                        onPositionChanged: mouse => { if (pressed) slider.pick(mouse.y) }
                    }
                }
            }

            ColumnLayout {
                Layout.alignment: Qt.AlignTop
                Layout.fillHeight: true
                spacing: 0

                Text {
                    text: qsTr("New")
                    color: root.look.label
                    font.pixelSize: root.look.captionSize
                }
                // The new color over the current one, in one rounded block: what changes is seen.
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 14 * root.u
                    Layout.topMargin: root.u / 2
                    Layout.bottomMargin: root.u / 2
                    Rectangle {
                        id: block
                        anchors.fill: parent
                        radius: root.look.smallCorner
                        color: root.original
                    }
                    AbstractButton { // the current color: a click puts it back
                        id: currentSwatch
                        objectName: "colorCurrent"
                        x: 0
                        y: parent.height / 2
                        width: parent.width
                        height: parent.height / 2
                        focusPolicy: Qt.StrongFocus
                        hoverEnabled: true
                        ToolTip.visible: hovered
                        ToolTip.delay: App.theme.look.tooltipDelay
                        ToolTip.text: qsTr("Go back to %1").arg(root.original)
                        Accessible.name: qsTr("Current color %1, click to go back to it").arg(root.original)
                        onClicked: root.setColor(root.original)
                        background: null
                    }
                    Rectangle {
                        id: fresh
                        objectName: "colorPreview"
                        width: parent.width
                        height: parent.height / 2
                        radius: root.look.smallCorner
                        color: root.color
                        Rectangle { // squares the bottom corners, as the two meet
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: parent.radius
                            color: parent.color
                        }
                    }
                    Rectangle { // the line between the two
                        y: parent.height / 2 - 0.5
                        width: parent.width
                        height: 1
                        color: Qt.rgba(0, 0, 0, root.look.light ? 0.25 : 0.5)
                    }
                    Rectangle { // a ring when the current color is hovered or has the keys
                        visible: currentSwatch.hovered || currentSwatch.activeFocus && FocusInput.keyboard
                        y: parent.height / 2
                        width: parent.width
                        height: parent.height / 2
                        radius: root.look.smallCorner
                        color: "transparent"
                        border.width: 2
                        border.color: currentSwatch.activeFocus && FocusInput.keyboard ? root.look.accent : root.look.text
                    }
                    Rectangle { // the block's edge
                        anchors.fill: parent
                        radius: root.look.smallCorner
                        color: "transparent"
                        border.width: 1
                        border.color: root.edge
                    }
                }
                Text {
                    text: qsTr("Current")
                    color: root.look.label
                    font.pixelSize: root.look.captionSize
                }

                Item { Layout.fillHeight: true }

                GridLayout {
                    columns: 2
                    columnSpacing: 2 * root.u
                    rowSpacing: root.u
                    ColorNumberField {
                        id: fieldH
                        objectName: "colorChannel-H"
                        name: "H"
                        accessibleName: qsTr("Hue, degrees")
                        maximum: 360
                        suffix: "°"
                        value: root.hsbShown.h
                        onEdited: n => { root.hue = n % 360; root.syncFields(false) }
                        onAccepted: root.ok()
                    }
                    ColorNumberField {
                        id: fieldR
                        objectName: "colorChannel-R"
                        name: "R"
                        accessibleName: qsTr("Red")
                        value: root.rgbShown.r
                        onEdited: n => root.chooseHex(root.history.hexFromRgb(n, root.rgbShown.g, root.rgbShown.b))
                        onAccepted: root.ok()
                    }
                    ColorNumberField {
                        id: fieldS
                        objectName: "colorChannel-S"
                        name: "S"
                        accessibleName: qsTr("Saturation, percent")
                        maximum: 100
                        suffix: "%"
                        value: root.hsbShown.s
                        onEdited: n => { root.sat = n / 100; root.syncFields(false) }
                        onAccepted: root.ok()
                    }
                    ColorNumberField {
                        id: fieldG
                        objectName: "colorChannel-G"
                        name: "G"
                        accessibleName: qsTr("Green")
                        value: root.rgbShown.g
                        onEdited: n => root.chooseHex(root.history.hexFromRgb(root.rgbShown.r, n, root.rgbShown.b))
                        onAccepted: root.ok()
                    }
                    ColorNumberField {
                        id: fieldB
                        objectName: "colorChannel-B"
                        name: "B"
                        accessibleName: qsTr("Brightness, percent")
                        maximum: 100
                        suffix: "%"
                        value: root.hsbShown.b
                        onEdited: n => { root.val = n / 100; root.syncFields(false) }
                        onAccepted: root.ok()
                    }
                    ColorNumberField {
                        id: fieldBlue
                        objectName: "colorChannel-Blue"
                        name: "B"
                        accessibleName: qsTr("Blue")
                        value: root.rgbShown.b
                        onEdited: n => root.chooseHex(root.history.hexFromRgb(root.rgbShown.r, root.rgbShown.g, n))
                        onAccepted: root.ok()
                    }
                }
            }
        }

        // The colors used lately; one click picks and closes.
        ColumnLayout {
            visible: root.showRecent && root.history && root.history.recent.length > 0
            Layout.fillWidth: true
            spacing: root.u
            Text {
                text: qsTr("Recent colors")
                color: root.look.label
                font.pixelSize: root.look.captionSize
            }
            Flow {
                id: recents
                objectName: "colorRecent"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                spacing: root.u
                Accessible.name: qsTr("Recent colors")
                Repeater {
                    model: root.history ? root.history.recent : []
                    delegate: AbstractButton {
                        id: chip
                        required property int index
                        required property string modelData
                        objectName: "colorRecent-" + index
                        implicitWidth: 6 * root.u
                        implicitHeight: 6 * root.u
                        focusPolicy: Qt.StrongFocus
                        hoverEnabled: true
                        Accessible.name: qsTr("Recent color %1").arg(modelData)
                        onClicked: root.confirm(chip.modelData)
                        background: Rectangle {
                            radius: root.look.smallCorner
                            color: chip.modelData
                            border.width: chip.activeFocus && FocusInput.keyboard || chip.hovered ? 2 : 1
                            border.color: chip.activeFocus && FocusInput.keyboard ? root.look.accent
                                        : chip.hovered ? root.look.text : root.edge
                        }
                    }
                }
            }
        }

        // The hex value at the start, the two answers at the end: Cancel, then OK (the one
        // highlighted control), as the other dialogs have them.
        RowLayout {
            spacing: root.u
            Text {
                text: qsTr("Hex")
                color: root.look.label
                font.pixelSize: root.look.labelSize
                Accessible.ignored: true
            }
            ThemedTextField {
                id: hexField
                objectName: "colorHex"
                Layout.preferredWidth: 22 * root.u
                Layout.preferredHeight: 7 * root.u
                font.family: "monospace"
                font.pixelSize: root.look.labelSize
                color: root.hexValid ? root.look.text : root.look.error
                maximumLength: 7
                selectByMouse: true
                Accessible.name: qsTr("Hex color")
                onTextEdited: {
                    const color = root.history.normalize(text)
                    if (color !== "")
                        root.chooseHex(color)
                }
                onAccepted: root.ok()
                onActiveFocusChanged: if (!activeFocus) root.syncFields(true)
            }
            Item { Layout.fillWidth: true }
            ViewButton {
                objectName: "colorCancel"
                look: root.look
                text: qsTr("Cancel")
                onTapped: root.canceled()
            }
            ViewButton {
                id: okButton
                objectName: "colorOk"
                look: root.look
                text: qsTr("OK")
                primary: true
                enabled: root.hexValid
                onTapped: root.ok()
            }
        }
    }
}
