pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes

// One node on the canvas: a NodeList row (canvas_model.h), drawn as docs/canvas-look.md says
// (4 anatomy, 5 ports, 9 states, 14 zoom tiers). Its ports sit where the model says (each
// port's `y`), so wires and hit tests agree with what is drawn. It takes no input itself:
// FugueCanvas handles every press and hover, so the delegate stays cheap at 300 nodes.
//
// States come from roles: mode (bypass, mute), missing, selected, and what a run set
// (runState, progress, seconds, error, warnings, thumbnail; #19 fills them through
// CanvasModel::setRunState). An unlinked input's widget is InputWell.qml (#48's slot).
Item {
    id: node

    required property string nodeId
    required property string title
    required property bool missing
    required property real posX
    required property real posY
    required property real nodeWidth
    required property real nodeHeight
    required property bool collapsed
    required property string mode // "normal", "bypass" or "mute"
    required property bool selected
    required property var inputs
    required property var outputs
    required property string color // layout color: a theme role, or empty
    required property bool pinned
    required property string runState // "", cached, stale, running, frozen, error, skipped
    required property real progress
    required property real seconds
    required property string error
    required property var warnings
    required property string thumbnail
    required property string thumbnailInfo
    required property rect thumbnailRect

    // Set by the canvas.
    property var canvasModel
    property var canvasView // the FugueCanvas: port colors
    property var look       // theme.look
    property var linkDrag: ({})
    property rect detailArea
    // 0: full detail; 1: the map (boxes, headers, port marks, thumbnails); 2: the overview
    // (no port marks either). Bound to the tier, never to the zoom.
    property int tier: 0
    property bool hovered: false
    property string hoverKey: "" // the port under the pointer: "o:<name>" or "i:<name>"

    // Port rows are made the first time the node is in view at the full or map tier and kept
    // after that, so a big graph opens at the cost of what is on screen.
    // From the model's values, not x and y, which can still be 0 while the delegate is made:
    // a node that only looked in view for that moment would make its rows for nothing.
    readonly property bool inView: posX < detailArea.x + detailArea.width && posX + nodeWidth > detailArea.x
                                   && posY < detailArea.y + detailArea.height && posY + nodeHeight > detailArea.y
    readonly property bool wanted: inView && tier <= 1
    property bool rowsMade: false
    onWantedChanged: if (wanted) rowsMade = true
    Component.onCompleted: if (wanted) rowsMade = true

    readonly property bool dragging: linkDrag.node !== undefined
    readonly property bool full: tier === 0
    readonly property real u: look.unit
    readonly property int corner: look.corner
    readonly property bool bypassed: mode === "bypass"
    readonly property bool muted: mode === "mute"
    readonly property bool running: runState === "running"
    readonly property bool failed: runState === "error"
    readonly property bool frozen: runState === "frozen" && !bypassed && !muted
    readonly property bool dashed: bypassed || missing
    readonly property color headerFill: failed ? look.errorHeader
                                       : color !== "" ? look.role(color).tint : look.header
    readonly property color edge: failed ? look.error
                                : running ? look.accent
                                : hovered ? look.hairlineStrong : look.hairline

    // The kind mark: the shape and color of the first output (a sink: its first input).
    readonly property var kindPort: outputs.length > 0 ? outputs[0] : inputs.length > 0 ? inputs[0] : null

    // What the header's meta says, besides the warnings and the pin: the mode first, then the
    // run's state.
    readonly property string stateGlyph: missing ? "circle-help"
                                       : bypassed ? "corner-down-right"
                                       : muted ? "ban"
                                       : runState === "cached" ? "check"
                                       : runState === "stale" ? "refresh-cw"
                                       : runState === "running" ? "loader-circle"
                                       : runState === "frozen" ? "snowflake"
                                       : runState === "error" ? "triangle-alert"
                                       : runState === "skipped" ? "skip-forward" : ""
    readonly property string caption: missing ? ""
                                    : bypassed ? qsTr("Bypassed")
                                    : muted ? qsTr("Muted")
                                    : runState === "running" && progress >= 0 ? Math.round(progress * 100) + "%"
                                    : runState === "cached" && seconds >= 0 ? formatSeconds(seconds)
                                    : runState === "frozen" ? qsTr("Frozen")
                                    : runState === "skipped" ? qsTr("Skipped") : ""
    function formatSeconds(s) {
        return s < 10 ? qsTr("%1 s").arg(s.toFixed(1)) : s < 60 ? qsTr("%1 s").arg(Math.round(s))
             : qsTr("%1 min").arg((s / 60).toFixed(1))
    }

    x: posX
    y: posY
    width: nodeWidth
    height: nodeHeight
    opacity: muted ? 0.4 : runState === "skipped" ? 0.6 : 1

    // Selected: a 2 px accent ring outside a 2 px gap; its corner r + 4 (0 when r is 0).
    Rectangle {
        visible: node.selected
        x: -4
        y: -4
        width: parent.width + 8
        height: parent.height + 8
        radius: node.corner > 0 ? node.corner + 4 : 0
        color: "transparent"
        border.width: 2
        border.color: node.look.accent
    }

    // The box: body fill and a 1 px edge (dashed for bypassed and missing nodes, below).
    Rectangle {
        anchors.fill: parent
        radius: node.corner
        color: node.look.body
        border.width: node.dashed ? 0 : 1
        border.color: node.edge
    }
    // The header: inside the edge, its top corners r - 1.
    Rectangle {
        x: 1
        y: 1
        width: parent.width - 2
        height: node.collapsed ? node.height - 2 : node.canvasModel.headerHeight - 1
        topLeftRadius: Math.max(0, node.corner - 1)
        topRightRadius: Math.max(0, node.corner - 1)
        bottomLeftRadius: node.collapsed ? Math.max(0, node.corner - 1) : 0
        bottomRightRadius: node.collapsed ? Math.max(0, node.corner - 1) : 0
        color: node.headerFill

        // Dark themes: a 1 px highlight along the top.
        Rectangle {
            visible: !node.look.light && node.full
            x: Math.max(0, node.corner - 1)
            width: parent.width - 2 * x
            height: 1
            color: node.look.bevel
        }
        // The kind mark, 8 px.
        Rectangle {
            id: kind
            readonly property string shape: node.kindPort ? node.kindPort.shape : ""
            visible: node.kindPort !== null
            x: 3 * node.u - 1
            anchors.verticalCenter: parent.verticalCenter
            width: 8
            height: 8
            radius: shape === "circle" || shape === "ring" ? 4 : Math.min(node.corner, 1.5)
            rotation: shape === "diamond" ? 45 : 0
            scale: shape === "diamond" ? 0.85 : 1
            color: shape !== "ring" && node.kindPort ? node.canvasView.portColor(node.kindPort.color) : "transparent"
            border.width: shape === "ring" ? 1.5 : 0
            border.color: node.kindPort ? node.canvasView.portColor(node.kindPort.color) : "transparent"
        }
        Text {
            visible: node.full
            x: kind.visible ? kind.x + kind.width + 1.5 * node.u : 3 * node.u - 1
            width: parent.width - x - (decor.item ? decor.item.metaWidth + 1.5 * node.u : 0) - 2.5 * node.u
            anchors.verticalCenter: parent.verticalCenter
            text: node.title
            elide: Text.ElideRight
            textFormat: Text.PlainText
            color: node.missing ? node.look.error : node.look.text
            opacity: node.bypassed ? 0.55 : 1
            font.pixelSize: node.look.textSize
            font.weight: Font.DemiBold
        }
    }

    // One row per port, outputs first (docs/canvas-look.md 4.3).
    Repeater {
        model: node.collapsed || !node.rowsMade ? 0 : node.outputs.length
        delegate: Item {
            id: output
            required property int index
            // Read from the node's own array: a Repeater over a list of maps would convert
            // the map again on every read.
            readonly property var modelData: node.outputs[index]
            readonly property bool dim: node.dragging
                && node.canvasModel.dragProblem(node.nodeId, modelData.name, true) !== ""
            readonly property bool under: node.hoverKey === "o:" + modelData.name
            y: modelData.rowTop
            width: node.width
            height: modelData.rowHeight
            opacity: dim ? 0.25 : 1

            Text {
                id: outputLabel
                visible: node.full
                anchors.right: parent.right
                anchors.rightMargin: 3 * node.u
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - 6 * node.u - (chip.item ? chip.item.width + 2 * node.u : 0))
                text: output.modelData.label
                elide: Text.ElideLeft
                textFormat: Text.PlainText
                color: node.look.label
                font.pixelSize: node.look.labelSize
                opacity: node.bypassed ? 0.55 : 1
            }
            // The last value, as a chip left of the label.
            Loader {
                id: chip
                active: node.full && output.modelData.value !== undefined && output.modelData.value !== ""
                anchors.right: outputLabel.left
                anchors.rightMargin: 2 * node.u
                anchors.verticalCenter: parent.verticalCenter
                sourceComponent: Rectangle {
                    width: Math.min(chipText.implicitWidth + 3 * node.u, node.width * 0.5)
                    height: chipText.implicitHeight + node.u
                    radius: node.look.smallCorner
                    color: node.look.well
                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        width: Math.min(implicitWidth, parent.width - 3 * node.u)
                        text: output.modelData.value
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        color: node.look.text
                        font.pixelSize: node.look.captionSize
                        font.features: { "tnum": 1 }
                    }
                }
            }
            PortDot {
                visible: node.tier <= 1
                x: node.width - width / 2
                y: node.canvasModel.rowHeight / 2 - height / 2
                shape: output.modelData.shape
                fill: node.canvasView.portColor(output.modelData.color)
                body: node.look.body
                corner: node.corner
                list: output.modelData.list
                wired: output.modelData.wired
                hovered: output.under && !node.dragging
                hot: output.under && node.dragging && !output.dim
            }
        }
    }

    Repeater {
        model: node.collapsed || !node.rowsMade ? 0 : node.inputs.length
        delegate: Item {
            id: input
            required property int index
            readonly property var modelData: node.inputs[index]
            readonly property bool dim: node.dragging
                && node.canvasModel.dragProblem(node.nodeId, modelData.name, false) !== ""
            readonly property bool under: node.hoverKey === "i:" + modelData.name
            readonly property bool showsWidget: modelData.widget && !modelData.wired
            y: modelData.rowTop
            width: node.width
            height: modelData.rowHeight
            opacity: dim ? 0.25 : 1

            Text {
                visible: node.full && !input.showsWidget
                x: 3 * node.u
                width: parent.width - 6 * node.u
                height: node.canvasModel.rowHeight
                verticalAlignment: Text.AlignVCenter
                text: input.showsWidget ? "" : input.modelData.label
                elide: Text.ElideRight
                textFormat: Text.PlainText
                color: node.look.label
                font.pixelSize: node.look.labelSize
                opacity: node.bypassed ? 0.55 : 1
            }
            // The widget (#48 makes it editable in InputWell.qml).
            Loader {
                active: node.full && input.showsWidget
                x: 3 * node.u
                y: Math.round(0.5 * node.u)
                width: parent.width - 6 * node.u
                height: parent.height - 2 * Math.round(0.5 * node.u)
                opacity: node.bypassed ? 0.55 : 1
                sourceComponent: InputWell {
                    port: input.modelData
                    nodeId: node.nodeId
                    canvasModel: node.canvasModel
                    look: node.look
                }
            }
            // An unlinked widget input shows its mark only on hover or to a compatible link.
            PortDot {
                visible: node.tier <= 1 && (!input.showsWidget || node.hovered || (node.dragging && !input.dim))
                x: -width / 2
                y: node.canvasModel.rowHeight / 2 - height / 2
                shape: input.modelData.shape
                fill: node.canvasView.portColor(input.modelData.color)
                body: node.look.body
                corner: node.corner
                list: input.modelData.list
                wired: input.modelData.wired
                hovered: input.under && !node.dragging
                hot: input.under && node.dragging && !input.dim
            }
        }
    }

    // The last image: thumbnail (docs/canvas-look.md 4.6), in every tier.
    Loader {
        active: node.thumbnail !== "" && !node.collapsed
        x: node.thumbnailRect.x
        y: node.thumbnailRect.y
        width: node.thumbnailRect.width
        height: node.thumbnailRect.height
        sourceComponent: Thumbnail {
            source: node.thumbnail
            info: node.thumbnailInfo
            look: node.look
            full: node.full
            // While a node runs, its previous result stays at half strength.
            opacity: node.running ? 0.5 : 1
        }
    }

    // Everything a state adds (docs/canvas-look.md 9): the header's meta (caption, warnings,
    // pin, state glyph), the running pulse and progress, dashed edges, the bypass line, the
    // placeholder's note and the error chip. Made only for a node that has one, so a plain
    // node stays a handful of items.
    Loader {
        id: decor
        active: node.stateGlyph !== "" || node.caption !== "" || node.warnings.length > 0 || node.pinned
                || node.dashed
        anchors.fill: parent
        sourceComponent: Item {
            readonly property real metaWidth: meta.visible ? meta.width : 0

            // While running the edge pulses (15), on the render thread.
            Rectangle {
                visible: node.running && node.full
                anchors.fill: parent
                radius: node.corner
                color: "transparent"
                border.width: 1
                border.color: node.look.accent
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    running: node.running && node.full
                    OpacityAnimator { from: 1; to: 0.45; duration: 1600; easing.type: Easing.InOutSine }
                    OpacityAnimator { from: 0.45; to: 1; duration: 1600; easing.type: Easing.InOutSine }
                }
            }
            // Bypassed and missing nodes: a dashed edge.
            Shape {
                visible: node.dashed
                anchors.fill: parent
                ShapePath {
                    strokeWidth: 1
                    strokeColor: node.missing ? node.look.error : node.look.hairlineStrong
                    strokeStyle: ShapePath.DashLine
                    dashPattern: [4, 3]
                    fillColor: "transparent"
                    PathRectangle {
                        x: 0.5
                        y: 0.5
                        width: node.width - 1
                        height: node.height - 1
                        radius: Math.max(0, node.corner - 0.5)
                    }
                }
            }
            // While running: progress along the header's bottom edge.
            Rectangle {
                visible: node.running && node.progress >= 0 && !node.collapsed
                x: 1
                y: node.canvasModel.headerHeight - 2
                width: node.width - 2
                height: 2
                color: node.look.hairline
                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, node.progress))
                    height: 2
                    color: node.look.accent
                }
            }
            Row {
                id: meta
                visible: node.full
                x: node.width - width - 2.5 * node.u
                y: ((node.collapsed ? node.height : node.canvasModel.headerHeight) - height) / 2
                spacing: node.u
                Text {
                    visible: text !== ""
                    anchors.verticalCenter: parent.verticalCenter
                    text: node.caption
                    color: node.frozen ? node.look.text : node.look.label
                    font.pixelSize: node.look.captionSize
                    font.features: { "tnum": 1 }
                }
                Text {
                    // Warnings: a count and the triangle, in the label color.
                    visible: node.warnings.length > 0
                    anchors.verticalCenter: parent.verticalCenter
                    text: node.warnings.length
                    color: node.look.label
                    font.pixelSize: node.look.captionSize
                }
                Glyph {
                    visible: node.warnings.length > 0
                    anchors.verticalCenter: parent.verticalCenter
                    name: "triangle-alert"
                    look: node.look
                    color: node.look.label
                }
                Glyph {
                    visible: node.pinned
                    anchors.verticalCenter: parent.verticalCenter
                    name: "pin"
                    look: node.look
                    color: node.look.label
                }
                Glyph {
                    visible: node.stateGlyph !== ""
                    anchors.verticalCenter: parent.verticalCenter
                    name: node.stateGlyph
                    look: node.look
                    color: node.failed || node.missing ? node.look.error : node.frozen ? node.look.text : node.look.label
                    RotationAnimator on rotation {
                        running: node.running && node.full
                        loops: Animation.Infinite
                        from: 0
                        to: 360
                        duration: 900
                    }
                }
            }

            // Bypassed (spec 3.5): a dashed line in the type color from the input to each
            // output it feeds.
            Repeater {
                model: node.bypassed && node.rowsMade && !node.collapsed && node.full ? node.outputs : []
                delegate: Shape {
                    id: pass
                    required property var modelData
                    readonly property var from: {
                        for (const p of node.inputs)
                            if (p.name === modelData.passFrom)
                                return p
                        return null
                    }
                    visible: from !== null
                    ShapePath {
                        strokeWidth: 2
                        strokeColor: node.canvasView.portColor(pass.modelData.color)
                        strokeStyle: ShapePath.DashLine
                        dashPattern: [2, 2]
                        fillColor: "transparent"
                        startX: 0
                        startY: pass.from ? pass.from.y : 0
                        PathCubic {
                            x: node.width
                            y: pass.modelData.y
                            control1X: node.width * 0.5
                            control1Y: pass.from ? pass.from.y : 0
                            control2X: node.width * 0.5
                            control2Y: pass.modelData.y
                        }
                    }
                }
            }

            // A placeholder's note (spec 11.6).
            Text {
                visible: node.missing && node.full && !node.collapsed
                x: 3 * node.u
                y: node.height - 2 * node.u - node.canvasModel.rowHeight
                width: node.width - 6 * node.u
                height: node.canvasModel.rowHeight
                verticalAlignment: Text.AlignVCenter
                text: qsTr("Not installed. Find it in packs…")
                elide: Text.ElideRight
                color: node.look.label
                font.pixelSize: node.look.captionSize
            }

            // In error: the message in a chip under the node (4.7).
            Rectangle {
                visible: node.failed && node.error !== "" && node.full
                y: node.height + Math.round(1.5 * node.u)
                width: node.width
                height: errorText.height + 3 * node.u
                radius: node.look.smallCorner
                color: node.look.errorChip
                Rectangle {
                    width: 2
                    height: parent.height
                    color: node.look.error
                }
                Glyph {
                    x: 2 * node.u
                    y: 1.5 * node.u
                    name: "triangle-alert"
                    look: node.look
                    color: node.look.error
                }
                Text {
                    id: errorText
                    x: 2 * node.u + 14 + 1.5 * node.u
                    y: 1.5 * node.u
                    width: parent.width - x - 2 * node.u
                    text: node.error
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    lineHeightMode: Text.FixedHeight
                    lineHeight: 15
                    color: node.look.text
                    font.pixelSize: node.look.labelSize
                }
            }
        }
    }
}
