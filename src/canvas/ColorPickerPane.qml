pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import Fugue

// A color picker (#52, docs/ui-ergonomics.md "Color picker"): a hue and saturation disk with a
// brightness bar beside it as in Blender's picker, a hex field with Apply, and the colors used
// lately. It is self-contained: it knows nothing of nodes, Frames or the canvas, so any surface
// can use it (a node's color widget, a settings page, the canvas's layout colors).
//
//   color       (read) the color being chosen now, "#rrggbb"; follows every drag and keystroke.
//   setColor(c) starts the picker on `c` ("#rrggbb", "#rgb" or without the "#"); a bad value is ignored.
//   picked(c)   the person confirmed `c`: Apply, Enter in the hex field, or a click on a recent
//               color. The color is also put first in the recent list (App.colorHistory), unless
//               `remember` is false. Nothing else changes: the caller decides what picking does.
//   remember    whether picked() adds the color to the recent list; true.
//   showRecent  whether the recent colors show; true.
//   focusField() puts the keyboard in the hex field.
//
// It sizes itself (implicitWidth, implicitHeight) and draws no surface of its own: ColorPickerPopup
// puts it on a popup. The recent list and the color math are App.colorHistory (menu/color_history.h).
Item {
    id: root

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property ColorHistory history: App.colorHistory
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft

    property bool remember: true
    property bool showRecent: true
    signal picked(string color)

    // The color being chosen, as the disk's hue (degrees), saturation and value (0 to 1).
    property real hue: 0
    property real sat: 1
    property real val: 1
    readonly property string color: history ? history.hex(hue, sat, val) : "#000000"

    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight

    function setColor(color) {
        const normal = history ? history.normalize(color) : ""
        if (normal === "")
            return
        const c = history.hsv(normal)
        hue = c.h
        sat = c.s
        val = c.v
        hexField.text = normal
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

    ColumnLayout {
        id: layout
        anchors.fill: parent
        spacing: 3 * root.u // a chunk apart: twice the 1u inside a chunk, and more
        LayoutMirroring.enabled: root.rightToLeft
        LayoutMirroring.childrenInherit: true

        // The disk and the brightness bar.
        RowLayout {
            spacing: 2 * root.u

            Item {
                id: disk
                objectName: "colorWheel"
                readonly property real size: 40 * root.u
                readonly property real radius: size / 2
                implicitWidth: size
                implicitHeight: size
                activeFocusOnTab: true
                Accessible.role: Accessible.Slider
                Accessible.name: qsTr("Hue and saturation")
                // Hue is the angle (0 at the right, counter-clockwise, as the gradient is drawn),
                // saturation the distance from the middle.
                function pick(x, y) {
                    const dx = x - radius, dy = radius - y
                    let angle = Math.atan2(dy, dx) * 180 / Math.PI
                    if (angle < 0)
                        angle += 360
                    root.hue = angle
                    root.sat = Math.min(1, Math.hypot(dx, dy) / radius)
                    hexField.text = root.color
                }
                Keys.onPressed: event => {
                    const step = event.modifiers & Qt.ShiftModifier ? 1 : 5
                    if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                        root.hue = (root.hue + (event.key === Qt.Key_Left ? step : -step) + 360) % 360
                    } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                        root.sat = Math.max(0, Math.min(1, root.sat + (event.key === Qt.Key_Up ? step : -step) / 100))
                    } else {
                        return
                    }
                    hexField.text = root.color
                    event.accepted = true
                }

                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        strokeWidth: -1
                        fillGradient: ConicalGradient {
                            centerX: disk.radius
                            centerY: disk.radius
                            angle: 0
                            GradientStop { position: 0 / 6; color: "#ff0000" }
                            GradientStop { position: 1 / 6; color: "#ffff00" }
                            GradientStop { position: 2 / 6; color: "#00ff00" }
                            GradientStop { position: 3 / 6; color: "#00ffff" }
                            GradientStop { position: 4 / 6; color: "#0000ff" }
                            GradientStop { position: 5 / 6; color: "#ff00ff" }
                            GradientStop { position: 6 / 6; color: "#ff0000" }
                        }
                        PathAngleArc {
                            centerX: disk.radius
                            centerY: disk.radius
                            radiusX: disk.radius
                            radiusY: disk.radius
                            startAngle: 0
                            sweepAngle: 360
                        }
                    }
                    // Toward the middle the hue washes out to white: saturation.
                    ShapePath {
                        strokeWidth: -1
                        fillGradient: RadialGradient {
                            centerX: disk.radius
                            centerY: disk.radius
                            centerRadius: disk.radius
                            focalX: centerX
                            focalY: centerY
                            GradientStop { position: 0; color: "#ffffffff" }
                            GradientStop { position: 1; color: "#00ffffff" }
                        }
                        PathAngleArc {
                            centerX: disk.radius
                            centerY: disk.radius
                            radiusX: disk.radius
                            radiusY: disk.radius
                            startAngle: 0
                            sweepAngle: 360
                        }
                    }
                }
                // The brightness bar's setting darkens the whole disk.
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "black"
                    opacity: 1 - root.val
                }
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "transparent"
                    border.width: disk.activeFocus ? 2 : 1
                    border.color: disk.activeFocus ? root.look.accent : root.look.hairlineStrong
                }
                // Where the color is: a ring in the text color over a dark one reads on any hue.
                Rectangle {
                    objectName: "colorWheelHandle"
                    readonly property real angle: root.hue * Math.PI / 180
                    x: disk.radius + Math.cos(angle) * root.sat * disk.radius - width / 2
                    y: disk.radius - Math.sin(angle) * root.sat * disk.radius - height / 2
                    width: 3 * root.u
                    height: width
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: "white"
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -1
                        z: -1
                        radius: width / 2
                        color: "transparent"
                        border.width: 1
                        border.color: "black"
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.CrossCursor
                    onPressed: mouse => { disk.forceActiveFocus(); disk.pick(mouse.x, mouse.y) }
                    onPositionChanged: mouse => { if (pressed) disk.pick(mouse.x, mouse.y) }
                }
            }

            // Brightness: the picked hue at the top, black at the bottom.
            Item {
                id: bar
                objectName: "colorBrightness"
                implicitWidth: 7 * root.u
                implicitHeight: disk.size
                activeFocusOnTab: true
                Accessible.role: Accessible.Slider
                Accessible.name: qsTr("Brightness")
                function pick(y) {
                    root.val = Math.max(0, Math.min(1, 1 - y / height))
                    hexField.text = root.color
                }
                Keys.onPressed: event => {
                    const step = (event.modifiers & Qt.ShiftModifier ? 1 : 5) / 100
                    if (event.key !== Qt.Key_Up && event.key !== Qt.Key_Down)
                        return
                    root.val = Math.max(0, Math.min(1, root.val + (event.key === Qt.Key_Up ? step : -step)))
                    hexField.text = root.color
                    event.accepted = true
                }
                Rectangle {
                    anchors.fill: parent
                    radius: root.look.smallCorner
                    border.width: bar.activeFocus ? 2 : 1
                    border.color: bar.activeFocus ? root.look.accent : root.look.hairlineStrong
                    gradient: Gradient {
                        GradientStop { position: 0; color: root.history ? root.history.hex(root.hue, root.sat, 1) : "white" }
                        GradientStop { position: 1; color: "black" }
                    }
                }
                Rectangle {
                    objectName: "colorBrightnessHandle"
                    y: (1 - root.val) * (bar.height - height)
                    width: parent.width
                    height: 3 * root.u
                    radius: root.look.smallCorner
                    color: "transparent"
                    border.width: 2
                    border.color: "white"
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -1
                        z: -1
                        radius: parent.radius
                        color: "transparent"
                        border.width: 1
                        border.color: "black"
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.SizeVerCursor
                    onPressed: mouse => { bar.forceActiveFocus(); bar.pick(mouse.y) }
                    onPositionChanged: mouse => { if (pressed) bar.pick(mouse.y) }
                }
            }
        }

        // The color as it will be, its hex, and Apply (the one highlighted control).
        RowLayout {
            spacing: root.u
            Rectangle {
                objectName: "colorPreview"
                implicitWidth: 7 * root.u
                implicitHeight: 7 * root.u
                radius: root.look.smallCorner
                color: root.color
                border.width: 1
                border.color: root.look.hairlineStrong
            }
            ThemedTextField {
                id: hexField
                objectName: "colorHex"
                Layout.fillWidth: true
                Layout.preferredHeight: 7 * root.u
                font.family: "monospace"
                maximumLength: 7
                placeholderText: "#rrggbb"
                selectByMouse: true
                Accessible.name: qsTr("Hex color")
                onTextEdited: {
                    const color = root.history.normalize(text)
                    if (color !== "") {
                        const c = root.history.hsv(color)
                        root.hue = c.h
                        root.sat = c.s
                        root.val = c.v
                    }
                }
                onAccepted: if (applyButton.enabled) applyButton.clicked()
            }
            ThemedButton {
                id: applyButton
                objectName: "colorApply"
                Layout.preferredHeight: 7 * root.u
                highlighted: true
                text: qsTr("Apply")
                enabled: root.history && root.history.normalize(hexField.text) !== ""
                onClicked: root.confirm(root.history.normalize(hexField.text))
            }
        }

        // The colors used lately; one click applies and closes.
        Flow {
            id: recents
            objectName: "colorRecent"
            visible: root.showRecent && root.history && root.history.recent.length > 0
            Layout.fillWidth: true
            Layout.preferredWidth: disk.size + 2 * root.u + 7 * root.u
            spacing: root.u
            Accessible.name: qsTr("Recent colors")
            Repeater {
                model: root.history ? root.history.recent : []
                delegate: AbstractButton {
                    id: chip
                    required property int index
                    required property string modelData
                    objectName: "colorRecent-" + index
                    implicitWidth: 7 * root.u
                    implicitHeight: 7 * root.u
                    focusPolicy: Qt.StrongFocus
                    Accessible.name: qsTr("Recent color %1").arg(modelData)
                    onClicked: root.confirm(chip.modelData)
                    background: Rectangle {
                        radius: root.look.smallCorner
                        color: chip.modelData
                        border.width: chip.activeFocus || chip.hovered ? 2 : 1
                        border.color: chip.activeFocus || chip.hovered ? root.look.text : root.look.hairlineStrong
                    }
                }
            }
        }
    }
}
