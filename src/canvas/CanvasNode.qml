pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import "widgets"

// One node on the canvas: a NodeList row (canvas_model.h), drawn as docs/canvas-look.md says
// (4 anatomy, 5 ports, 9 states, 14 zoom tiers). Its ports sit where the model says (each
// port's `y`), so wires and hit tests agree with what is drawn. It takes no input itself:
// FugueCanvas handles every press and hover, so the delegate stays cheap at 300 nodes.
//
// States come from roles: mode (bypass, mute), missing, selected, and what a run set
// (runState, progress, seconds, error, warnings, thumbnail; #19 fills them through
// CanvasModel::setRunState). A value input's widget is widgets/InputWidget.qml (#48).
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
    property bool seen: false
    onInViewChanged: if (inView) seen = true
    Component.onCompleted: {
        if (inView)
            seen = true
        if (wanted)
            rowsMade = true
    }

    objectName: "node-" + nodeId
    x: posX
    y: posY
    width: nodeWidth
    height: nodeHeight
    opacity: mode === "mute" ? 0.4 : runState === "skipped" ? 0.6 : 1

    // The node's face is made the first time it is in view (the detail area, at any zoom) and
    // kept after that: off screen a node is two items, so a big graph opens at the cost of what
    // is on screen (12.1).
    Loader {
        active: node.seen
        anchors.fill: parent
        sourceComponent: Item {
            id: face
            anchors.fill: parent

            readonly property bool dragging: node.linkDrag.node !== undefined
            readonly property bool full: node.tier === 0
            readonly property real u: node.look.unit
            readonly property int corner: node.look.corner
            readonly property bool bypassed: node.mode === "bypass"
            readonly property bool muted: node.mode === "mute"
            readonly property bool running: node.runState === "running"
            readonly property bool failed: node.runState === "error"
            readonly property bool frozen: node.runState === "frozen" && !bypassed && !muted
            readonly property bool dashed: bypassed || node.missing
            readonly property color headerFill: failed ? node.look.errorHeader
                                               : node.color !== "" ? node.look.role(node.color).tint : node.look.header
            readonly property color edge: failed ? node.look.error
                                        : running ? node.look.accent
                                        : node.hovered ? node.look.hairlineStrong : node.look.hairline

            // The kind mark: the shape and color of the first output (a sink: its first input).
            readonly property var kindPort: node.outputs.length > 0 ? node.outputs[0]
                                          : node.inputs.length > 0 ? node.inputs[0] : null

            // What the header's meta says, besides the warnings and the pin: the mode first,
            // then the run's state.
            readonly property string stateGlyph: node.missing ? "circle-help"
                                               : bypassed ? "corner-down-right"
                                               : muted ? "ban"
                                               : node.runState === "cached" ? "check"
                                               : node.runState === "stale" ? "refresh-cw"
                                               : node.runState === "running" ? "loader-circle"
                                               : node.runState === "frozen" ? "snowflake"
                                               : node.runState === "error" ? "triangle-alert"
                                               : node.runState === "skipped" ? "skip-forward" : ""
            readonly property string caption: node.missing ? ""
                                            : bypassed ? qsTr("Bypassed")
                                            : muted ? qsTr("Muted")
                                            : node.runState === "running" && node.progress >= 0
                                              ? Math.round(node.progress * 100) + "%"
                                            : node.runState === "cached" && node.seconds >= 0
                                              ? formatSeconds(node.seconds)
                                            : node.runState === "frozen" ? qsTr("Frozen")
                                            : node.runState === "skipped" ? qsTr("Skipped") : ""
            function formatSeconds(s) {
                return s < 10 ? qsTr("%1 s").arg(s.toFixed(1)) : s < 60 ? qsTr("%1 s").arg(Math.round(s))
                     : qsTr("%1 min").arg((s / 60).toFixed(1))
            }

            // Selected: a 2 px accent ring outside a 2 px gap; its corner r + 4 (0 when r is 0).
            Rectangle {
                visible: node.selected
                x: -4
                y: -4
                width: parent.width + 8
                height: parent.height + 8
                radius: face.corner > 0 ? face.corner + 4 : 0
                color: "transparent"
                border.width: 2
                border.color: node.look.accent
            }

            // The box: body fill and a 1 px edge (dashed for bypassed and missing nodes, below).
            Rectangle {
                anchors.fill: parent
                radius: face.corner
                color: node.look.body
                border.width: face.dashed ? 0 : 1
                border.color: face.edge
            }
            // The header: inside the edge, its top corners r - 1.
            Rectangle {
                x: 1
                y: 1
                width: parent.width - 2
                height: node.collapsed ? node.height - 2 : node.canvasModel.headerHeight - 1
                topLeftRadius: Math.max(0, face.corner - 1)
                topRightRadius: Math.max(0, face.corner - 1)
                bottomLeftRadius: node.collapsed ? Math.max(0, face.corner - 1) : 0
                bottomRightRadius: node.collapsed ? Math.max(0, face.corner - 1) : 0
                color: face.headerFill

                // Dark themes: a 1 px highlight along the top.
                Rectangle {
                    visible: !node.look.light && face.full
                    x: Math.max(0, face.corner - 1)
                    width: parent.width - 2 * x
                    height: 1
                    color: node.look.bevel
                }
                // The kind mark, 8 px.
                Rectangle {
                    id: kind
                    readonly property string shape: face.kindPort ? face.kindPort.shape : ""
                    visible: face.kindPort !== null
                    x: 3 * face.u - 1
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8
                    height: 8
                    radius: shape === "circle" || shape === "ring" ? 4 : Math.min(face.corner, 1.5)
                    rotation: shape === "diamond" ? 45 : 0
                    scale: shape === "diamond" ? 0.85 : 1
                    color: shape !== "ring" && face.kindPort ? node.canvasView.portColor(face.kindPort.color) : "transparent"
                    border.width: shape === "ring" ? 1.5 : 0
                    border.color: face.kindPort ? node.canvasView.portColor(face.kindPort.color) : "transparent"
                }
                Text {
                    visible: face.full
                    x: kind.visible ? kind.x + kind.width + 1.5 * face.u : 3 * face.u - 1
                    width: parent.width - x - (decor.item ? decor.item.metaWidth + 1.5 * face.u : 0) - 2.5 * face.u
                    anchors.verticalCenter: parent.verticalCenter
                    text: node.title
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    color: node.missing ? node.look.error : node.look.text
                    opacity: face.bypassed ? 0.55 : 1
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
                    readonly property bool dim: face.dragging
                        && node.canvasModel.dragProblem(node.nodeId, modelData.name, true) !== ""
                    readonly property bool under: node.hoverKey === "o:" + modelData.name
                    y: modelData.rowTop
                    width: node.width
                    height: modelData.rowHeight
                    opacity: dim ? 0.25 : 1

                    Text {
                        id: outputLabel
                        visible: face.full
                        anchors.right: parent.right
                        anchors.rightMargin: 3 * face.u
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width - 6 * face.u - (chip.item ? chip.item.width + 2 * face.u : 0))
                        text: output.modelData.label
                        elide: Text.ElideLeft
                        textFormat: Text.PlainText
                        color: node.look.label
                        font.pixelSize: node.look.labelSize
                        opacity: face.bypassed ? 0.55 : 1
                    }
                    // The last value, as a chip left of the label.
                    Loader {
                        id: chip
                        active: face.full && output.modelData.value !== undefined && output.modelData.value !== ""
                        anchors.right: outputLabel.left
                        anchors.rightMargin: 2 * face.u
                        anchors.verticalCenter: parent.verticalCenter
                        sourceComponent: Rectangle {
                            width: Math.min(chipText.implicitWidth + 3 * face.u, node.width * 0.5)
                            height: chipText.implicitHeight + face.u
                            radius: node.look.smallCorner
                            color: node.look.well
                            Text {
                                id: chipText
                                anchors.centerIn: parent
                                width: Math.min(implicitWidth, parent.width - 3 * face.u)
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
                        corner: face.corner
                        list: output.modelData.list
                        wired: output.modelData.wired
                        hovered: output.under && !face.dragging
                        hot: output.under && face.dragging && !output.dim
                    }
                }
            }

            Repeater {
                model: node.collapsed || !node.rowsMade ? 0 : node.inputs.length
                delegate: Item {
                    id: input
                    required property int index
                    readonly property var modelData: node.inputs[index]
                    readonly property bool dim: face.dragging
                        && node.canvasModel.dragProblem(node.nodeId, modelData.name, false) !== ""
                    readonly property bool under: node.hoverKey === "i:" + modelData.name
                    // A value input's row has its widget (widgets/InputWidget.qml, #48) across it, the
                    // label drawn over the widget's well; a linked one shows where its value comes from.
                    readonly property bool hasWidget: modelData.widget
                    readonly property bool showsWidget: hasWidget && !modelData.wired
                    readonly property bool tall: showsWidget && modelData.widgetType === "multiline"
                    y: modelData.rowTop
                    width: node.width
                    height: modelData.rowHeight
                    opacity: dim ? 0.25 : 1

                    Loader {
                        objectName: "widgetLoader"
                        active: input.hasWidget
                        visible: face.full
                        // Under the label, except while typed into.
                        z: item && item.editingText ? 1 : -1
                        x: 3 * face.u
                        y: Math.round(0.5 * face.u)
                        width: node.width - 6 * face.u
                        height: input.height - 2 * Math.round(0.5 * face.u)
                        opacity: face.bypassed ? 0.55 : 1
                        sourceComponent: InputWidget {
                            edits: node.canvasModel.widgets
                            nodeId: node.nodeId
                            port: input.modelData
                            look: node.look
                            canvasView: node.canvasView
                            nodeItem: face
                            row: input
                        }
                    }
                    Text {
                        // A tall (multiline) well shows its text instead, and the label only while empty.
                        visible: face.full && (!input.tall || input.modelData.value === "" || input.modelData.value === undefined)
                        x: input.showsWidget ? 5 * face.u : 3 * face.u
                        y: input.tall ? Math.round(2 * face.u) : 0
                        width: (input.hasWidget ? 0.45 : 1) * (node.width - 6 * face.u) - (input.showsWidget ? 2 * face.u : 0)
                        height: input.tall ? 16 : node.canvasModel.rowHeight
                        verticalAlignment: Text.AlignVCenter
                        text: visible ? input.modelData.label : ""
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        color: node.look.label
                        font.pixelSize: node.look.labelSize
                        opacity: face.bypassed ? 0.55 : 1
                    }
                    // An unlinked widget input shows its mark only on hover or to a compatible link.
                    PortDot {
                        visible: node.tier <= 1 && (!input.showsWidget || node.hovered || (face.dragging && !input.dim))
                        x: -width / 2
                        y: node.canvasModel.rowHeight / 2 - height / 2
                        shape: input.modelData.shape
                        fill: node.canvasView.portColor(input.modelData.color)
                        body: node.look.body
                        corner: face.corner
                        list: input.modelData.list
                        wired: input.modelData.wired
                        hovered: input.under && !face.dragging
                        hot: input.under && face.dragging && !input.dim
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
                objectName: "thumbnail"
                sourceComponent: Thumbnail {
                    source: node.thumbnail
                    info: node.thumbnailInfo
                    look: node.look
                    full: face.full
                    // While a node runs, its previous result stays at half strength.
                    opacity: face.running ? 0.5 : 1
                }
            }
            // The resize grip (#49): two short diagonals in the bottom right corner, shown while the
            // node is hovered or selected. FugueCanvas takes the press and the double click.
            Loader {
                active: face.full && !node.collapsed && (node.hovered || node.selected)
                x: parent.width - width - 2
                y: parent.height - height - 2
                width: 10
                height: 10
                sourceComponent: Item {
                    Repeater {
                        model: 2
                        Rectangle {
                            required property int index
                            width: index === 0 ? 10 : 6
                            height: 1
                            x: index === 0 ? 0 : 4
                            y: index === 0 ? 10 : 10
                            transformOrigin: Item.BottomRight
                            rotation: -45
                            color: node.look.label
                            opacity: 0.8
                        }
                    }
                }
            }

            // Everything a state adds (docs/canvas-look.md 9): the header's meta (caption, warnings,
            // pin, state glyph), the running pulse and progress, dashed edges, the bypass line, the
            // placeholder's note, the error chip and a collapsed node's port marks. Made only for a
            // node that has one, so a plain node stays a handful of items.
            Loader {
                id: decor
                active: face.stateGlyph !== "" || face.caption !== "" || node.warnings.length > 0 || node.pinned
                        || face.dashed || node.collapsed
                anchors.fill: parent
                sourceComponent: Item {
                    readonly property real metaWidth: meta.visible ? meta.width : 0

                    // Collapsed: every input meets at the header's left end and every output at its
                    // right (docs/canvas-look.md 5), one mark a side in the first port's shape and color.
                    Repeater {
                        model: node.collapsed && node.tier <= 1 ? [node.inputs, node.outputs] : []
                        delegate: PortDot {
                            required property var modelData
                            required property int index
                            visible: modelData.length > 0
                            x: (index === 1 ? node.width : 0) - width / 2
                            y: node.height / 2 - height / 2
                            shape: modelData.length > 0 ? modelData[0].shape : "circle"
                            fill: modelData.length > 0 ? node.canvasView.portColor(modelData[0].color) : "transparent"
                            body: node.look.body
                            corner: face.corner
                            wired: modelData.some(p => p.wired)
                        }
                    }

                    // While running the edge pulses (15), on the render thread.
                    Rectangle {
                        visible: face.running && face.full
                        anchors.fill: parent
                        radius: face.corner
                        color: "transparent"
                        border.width: 1
                        border.color: node.look.accent
                        SequentialAnimation on opacity {
                            loops: Animation.Infinite
                            running: face.running && face.full
                            OpacityAnimator { from: 1; to: 0.45; duration: 1600; easing.type: Easing.InOutSine }
                            OpacityAnimator { from: 0.45; to: 1; duration: 1600; easing.type: Easing.InOutSine }
                        }
                    }
                    // Bypassed and missing nodes: a dashed edge.
                    Shape {
                        visible: face.dashed
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
                                radius: Math.max(0, face.corner - 0.5)
                            }
                        }
                    }
                    // While running: progress along the header's bottom edge.
                    Rectangle {
                        visible: face.running && node.progress >= 0 && !node.collapsed
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
                        visible: face.full
                        x: node.width - width - 2.5 * face.u
                        y: ((node.collapsed ? node.height : node.canvasModel.headerHeight) - height) / 2
                        spacing: face.u
                        Text {
                            visible: text !== ""
                            anchors.verticalCenter: parent.verticalCenter
                            text: face.caption
                            color: face.frozen ? node.look.text : node.look.label
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
                            visible: face.stateGlyph !== ""
                            anchors.verticalCenter: parent.verticalCenter
                            name: face.stateGlyph
                            look: node.look
                            color: face.failed || node.missing ? node.look.error : face.frozen ? node.look.text : node.look.label
                            RotationAnimator on rotation {
                                running: face.running && face.full
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
                        model: face.bypassed && node.rowsMade && !node.collapsed && face.full ? node.outputs : []
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
                        visible: node.missing && face.full && !node.collapsed
                        x: 3 * face.u
                        y: node.height - 2 * face.u - node.canvasModel.rowHeight
                        width: node.width - 6 * face.u
                        height: node.canvasModel.rowHeight
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Not installed. Find it in packs…")
                        elide: Text.ElideRight
                        color: node.look.label
                        font.pixelSize: node.look.captionSize
                    }

                    // In error: the message in a chip under the node (4.7).
                    Rectangle {
                        visible: face.failed && node.error !== "" && face.full
                        y: node.height + Math.round(1.5 * face.u)
                        width: node.width
                        height: errorText.height + 3 * face.u
                        radius: node.look.smallCorner
                        color: node.look.errorChip
                        Rectangle {
                            width: 2
                            height: parent.height
                            color: node.look.error
                        }
                        Glyph {
                            x: 2 * face.u
                            y: 1.5 * face.u
                            name: "triangle-alert"
                            look: node.look
                            color: node.look.error
                        }
                        Text {
                            id: errorText
                            x: 2 * face.u + 14 + 1.5 * face.u
                            y: 1.5 * face.u
                            width: parent.width - x - 2 * face.u
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
    }
}
