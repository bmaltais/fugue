import QtQuick

// An unlinked input's widget on a node (spec 6.2, docs/canvas-look.md 4.4): a recessed well
// across the row, its label on the left and its value on the right, drawn the way each
// widget kind looks. Read-only here; widget editing (#48) makes it editable in this file.
//
// The contract with CanvasNode.qml, which makes one per unlinked value input:
//   port        the input's port map (CanvasModel::portMap): name, label, widgetKind,
//               value, isDefault, share (a slider's fill), on (a toggle's state), ...
//   nodeId      the node's id, and canvasModel the CanvasModel, for edits
//   look        theme.look (look.h)
// The node sizes and places it (3u in from both sides, 0.5u above and below, the row's
// height). A multiline well is the 16u row; every other well is one 6u row. Only the canvas's
// zoom tier hides it. Nothing here takes input yet: FugueCanvas handles every press.
//
// There can be a hundred wells in view, so the common kinds (spin, line, choice, seed, file,
// folder) are four items, and what only some kinds draw is made by `extra` for those alone.
Rectangle {
    id: well

    required property var port
    required property string nodeId
    property var canvasModel
    required property var look

    readonly property string kind: port.widgetKind || "line"
    readonly property string value: port.value !== undefined ? String(port.value) : ""
    readonly property real u: look.unit
    readonly property bool oneLine: kind !== "multiline" && kind !== "model"
    // Room the extra part takes at the right (a switch or a swatch).
    readonly property real extraWidth: kind === "toggle" ? 22 : kind === "color" ? 12 : 0
    readonly property string trailGlyph: kind === "choice" || kind === "model" ? "chevron-down"
                                       : kind === "seed" ? "dices"
                                       : kind === "file" ? "file"
                                       : kind === "folder" ? "folder" : ""

    radius: look.smallCorner
    color: look.well
    clip: kind === "multiline"

    Loader {
        id: extra
        active: well.kind === "slider" || well.kind === "toggle" || well.kind === "color"
                || well.kind === "multiline" || well.kind === "model"
        anchors.fill: parent
        sourceComponent: well.kind === "slider" ? sliderFill
                       : well.kind === "toggle" ? toggleSwitch
                       : well.kind === "color" ? swatch
                       : well.kind === "multiline" ? multiline : modelPicker
    }

    Text {
        id: label
        visible: well.oneLine
        x: 2 * well.u
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, (well.width - 4 * well.u) * 0.55)
        text: visible ? well.port.label : ""
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: well.look.label
        font.pixelSize: well.look.labelSize
    }
    Text {
        visible: well.oneLine && well.kind !== "toggle"
        x: label.x + label.width + 1.5 * well.u
        anchors.verticalCenter: parent.verticalCenter
        width: well.width - x - 2 * well.u - (trail.visible ? trail.width + 1.5 * well.u : 0)
               - (well.extraWidth > 0 ? well.extraWidth + 1.5 * well.u : 0)
        horizontalAlignment: Text.AlignRight
        text: visible ? well.value : ""
        textFormat: Text.PlainText
        elide: well.kind === "file" || well.kind === "folder" ? Text.ElideMiddle : Text.ElideRight
        color: well.port.isDefault ? well.look.label : well.look.text
        font.pixelSize: well.look.labelSize
        font.weight: Font.Medium
        font.features: { "tnum": 1 }
    }
    Glyph {
        id: trail
        visible: name !== ""
        x: well.width - 2 * well.u - width
        anchors.verticalCenter: parent.verticalCenter
        name: well.trailGlyph
        look: well.look
        color: well.look.label
        font.pixelSize: 12
    }

    // A slider fills its value's share of the range at accent 22%.
    Component {
        id: sliderFill
        Item {
            Rectangle {
                width: well.width * (well.port.share || 0)
                height: well.height
                radius: well.radius
                color: well.look.accent
                opacity: 0.22
            }
        }
    }
    // A toggle: a 22 × 12 switch at the right.
    Component {
        id: toggleSwitch
        Item {
            Rectangle {
                readonly property bool on: well.port.on === true
                x: well.width - 2 * well.u - width
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                height: 12
                radius: 6
                color: on ? well.look.accent : well.look.hairlineStrong
                Rectangle {
                    x: parent.on ? parent.width - width - 2 : 2
                    y: 2
                    width: 8
                    height: 8
                    radius: 4
                    color: parent.on ? well.look.accentForeground : well.look.body
                }
            }
        }
    }
    // A color: its #rrggbb (the value) and a 12 px swatch at the right.
    Component {
        id: swatch
        Item {
            Rectangle {
                visible: /^#[0-9a-fA-F]{6,8}$/.test(well.value)
                x: well.width - 2 * well.u - width
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                radius: Math.min(well.look.smallCorner, 2)
                color: visible ? well.value : "transparent"
                border.width: 1
                border.color: well.look.hairline
            }
        }
    }
    // A multiline value: up to three lines at 16 px line height, from the top.
    Component {
        id: multiline
        Item {
            Text {
                x: 2 * well.u
                y: 1.5 * well.u
                width: well.width - 4 * well.u
                height: well.height - 3 * well.u
                text: well.value !== "" ? well.value : well.port.label
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                elide: Text.ElideRight
                maximumLineCount: Math.max(1, Math.floor(height / 16))
                lineHeightMode: Text.FixedHeight
                lineHeight: 16
                color: well.value !== "" && !well.port.isDefault ? well.look.text : well.look.label
                font.pixelSize: well.look.labelSize
            }
        }
    }
    // The model picker (resource inputs): a model glyph, the model's name, a chevron.
    Component {
        id: modelPicker
        Item {
            Glyph {
                id: modelGlyph
                x: 2 * well.u
                anchors.verticalCenter: parent.verticalCenter
                name: "box"
                look: well.look
                color: well.look.label
                font.pixelSize: 12
            }
            Text {
                x: modelGlyph.x + modelGlyph.width + 1.5 * well.u
                anchors.verticalCenter: parent.verticalCenter
                width: well.width - x - 2 * well.u - trail.width - 1.5 * well.u
                text: well.value !== "" ? well.value : qsTr("Any ready model")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: well.value !== "" ? well.look.text : well.look.label
                font.pixelSize: well.look.labelSize
                font.weight: Font.Medium
            }
        }
    }
}
