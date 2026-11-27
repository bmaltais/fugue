pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import Fugue
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
    required property int stack // where it stands among overlapping nodes: the last selected or moved is on top
    required property var inputs
    required property var outputs
    required property string color // layout color: a theme role, or empty
    required property bool pinned
    required property string runState // "", cached, stale, running, frozen, error, skipped
    required property real progress
    required property real seconds
    required property string error
    required property string errorDetail // the whole text when `error` is the short words of a missing pack (#264)
    required property var errorPack      // a node of a pack that is not installed: {id, name, text, installable}, else empty
    required property var warnings
    required property string notice // why it cannot run now, and the fix (8.5), from a check
    required property string runsOn // "<service> (<host>)" for a node routed to a service off this machine (8.2, #404), else empty
    required property string thumbnail
    required property string thumbnailInfo
    required property string thumbnailFile
    required property rect thumbnailRect
    required property string thumbnailText // in the image's place (a shown text, a label)
    required property bool thumbnailDim    // half strength: a placeholder, or the last run's while running
    required property var compare          // Compare Images: its result's pictures (#62), else empty
    required property var compareView      // its viewer's state
    required property var media            // a video or sound result's player (#116): {type, url, label, file, ...}, else empty
    required property string updateAvailable // a group the library has a newer version of (5.3)
    required property string newerMajor // a node on an older installed major: the newest version to move to (3.6)
    required property int groupSize // a group instance: the nodes it holds (docs/canvas-look.md 9), else 0

    z: stack // overlapping nodes: the one selected or moved last is in front

    // Set by the canvas.
    property CanvasModel canvasModel
    property var canvasView // the FugueCanvas: port colors
    property CanvasLook look       // App.theme.look
    property var linkDrag: ({})
    property rect detailArea
    // 0: full detail; 1: the map (boxes, headers, port marks, thumbnails); 2: the overview
    // (no port marks either). Bound to the tier, never to the zoom.
    property int tier: 0
    property bool hovered: false
    // The hover help of a port row: its label in bold, then its description, as rich text.
    function portTip(port) {
        const escape = t => String(t).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        return "<b>" + escape(port.label) + "</b><br>" + escape(port.description || "")
    }
    property string hoverKey: "" // the port under the pointer: "o:<name>" or "i:<name>"
    property string activeCorner: "" // the resize corner under the pointer or being dragged

    // Port rows are made the first time the node is in view at the full or map tier and kept
    // after that, so a big graph opens at the cost of what is on screen.
    // From the model's values, not x and y, which can still be 0 while the delegate is made:
    // a node that only looked in view for that moment would make its rows for nothing.
    readonly property bool inView: posX < detailArea.x + detailArea.width && posX + nodeWidth > detailArea.x
                                   && posY < detailArea.y + detailArea.height && posY + nodeHeight > detailArea.y
    readonly property bool wanted: inView && tier <= 1
    property bool rowsMade: false
    onWantedChanged: if (wanted) rowsMade = true
    onRowsMadeChanged: if (rowsMade) canvasModel.noteRowsMade(nodeId) // previews are made from here (#19)
    property var compareItem: null // the viewer on the face while it is made (Loader below)
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
            // The node style (docs/canvas-look.md 20.14, #466): Compact is the Drift card, Flat today's face. The model, the ports, the
            // wires and the gestures are the same; what differs is drawn here and in the widgets, and the metrics in CanvasModel.
            readonly property bool compact: node.look.nodeStyle === "compact"
            readonly property var pm: node.look.params
            readonly property int corner: compact ? Math.round(pm.cornerGroup * u) : node.look.corner
            // A card's side padding: 2.5u (the board's 10px), scaled by the Spacing slider; Flat keeps 3u.
            readonly property real padX: compact ? Math.round(2.5 * u * pm.rowPadRight / 3) : 3 * u
            readonly property int labelPx: compact ? node.look.textSize : node.look.labelSize
            readonly property int titlePx: compact ? node.look.textSize + 1 : node.look.textSize
            // Where a row's control starts in Compact: its label on the left, the control right of this.
            // It is a third of the card at least and half at most, wider for the longest label of a row with a control (a label is
            // about 6.4 px a character at 12 px).
            readonly property real splitX: {
                let longest = 0
                for (const p of node.inputs)
                    if (p.widget && !p.hidden && p.widgetType !== "multiline")
                        longest = Math.max(longest, String(p.label).length)
                return Math.round(Math.min(node.width * 0.5, Math.max(node.width * 0.36, padX + longest * 6.4 + 8)))
            }
            readonly property bool hairlines: compact && pm.rowSeparators === "hairline"
            readonly property bool bypassed: node.mode === "bypass"
            readonly property bool muted: node.mode === "mute"
            readonly property bool running: node.runState === "running"
            readonly property bool failed: node.runState === "error"
            readonly property bool frozen: node.runState === "frozen" && !bypassed && !muted
            readonly property bool dashed: bypassed || node.missing
            readonly property bool renaming: !!node.canvasView && node.canvasView.renamingNode === node.nodeId
            // A layout color tints the whole node, header and body, as in ComfyUI (#72).
            readonly property var colorRole: node.color !== "" ? node.look.role(node.color) : null
            readonly property color headerFill: failed ? node.look.errorHeader
                                               : bypassed ? node.look.bypassHeader
                                               : colorRole ? colorRole.tint
                                               : compact ? (node.look.light ? Qt.rgba(0, 0, 0, 0.025) : Qt.rgba(1, 1, 1, 0.03))
                                               : node.look.header
            readonly property color bodyFill: bypassed ? node.look.bypassBody
                                             : colorRole ? colorRole.tintBody
                                             : compact ? Qt.tint(node.look.body, node.look.kitGroup) : node.look.body
            readonly property color edge: failed ? node.look.error
                                        : running || (compact && node.selected) ? node.look.accent
                                        : node.hovered ? node.look.hairlineStrong
                                        : compact && pm.borders === "none" ? "transparent" : node.look.hairline

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
            // Around a group's whole stack, its second card included (#72).
            // Compact: the card's own edge turns accent and a 3 px ring of the accent at 18% surrounds it.
            Rectangle {
                readonly property real stack: node.groupSize > 0 ? face.u : 0
                readonly property real reach: face.compact ? 3 : 4
                visible: node.selected
                x: -reach
                y: -reach
                width: parent.width + 2 * reach + stack
                height: parent.height + 2 * reach + stack
                radius: face.corner > 0 ? face.corner + reach : 0
                color: "transparent"
                border.width: face.compact ? 3 : 2
                border.color: face.compact ? Qt.alpha(node.look.accent, 0.18) : node.look.accent
            }
            // Compact: the card's own extras (its shadow, the chevron and the hairlines) are made only while the style is Compact, so a Flat
            // node is the items it always was (12.1; test_canvas_window counts them). Made by compactParts below.
            property Item compactUnder: null
            property Item compactOver: null
            function syncCompact() {
                if (compact && !compactUnder) {
                    compactUnder = underParts.createObject(face)
                    compactOver = overParts.createObject(face)
                } else if (!compact && compactUnder) {
                    compactUnder.destroy()
                    compactOver.destroy()
                    compactUnder = null
                    compactOver = null
                }
            }
            onCompactChanged: syncCompact()
            Component.onCompleted: syncCompact()
            // Under the card: its soft shadow (the look's level 1, 2 selected), a 9-slice made once.
            Component {
                id: underParts
                Item {
                    anchors.fill: parent
                    z: -1
                    Shadow {
                        target: box
                        level: node.selected ? 2 : 1
                        look: node.look
                    }
                }
            }
            // Over the card: the hairline under the header, the fold chevron at its end (a click folds or unfolds the node, the canvas's
            // Collapse), and the hairline between the outputs and the inputs, inset by the card's padding.
            Component {
                id: overParts
                Item {
                    anchors.fill: parent
                    z: 1
                    Rectangle {
                        visible: face.hairlines && !node.collapsed && face.full
                        x: 1
                        y: node.canvasModel.headerHeight - 1
                        width: node.width - 2
                        height: 1
                        color: node.look.kitSeparator
                    }
                    Glyph {
                        objectName: "foldChevron"
                        visible: face.full
                        x: node.width - width - face.padX + 2
                        y: (node.canvasModel.headerHeight - height) / 2
                        look: node.look
                        name: node.collapsed ? "chevron-right" : "chevron-down"
                        color: node.look.label
                        font.pixelSize: 14
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                node.canvasModel.select([node.nodeId], "replace")
                                node.canvasModel.toggleCollapsed()
                            }
                        }
                    }
                    Rectangle {
                        visible: face.hairlines && face.full && node.rowsMade && !node.collapsed && node.outputs.length > 0 && node.inputs.length > 0
                        x: face.padX
                        y: node.outputs.length > 0 ? node.outputs[node.outputs.length - 1].rowTop + node.outputs[node.outputs.length - 1].rowHeight : 0
                        width: node.width - 2 * face.padX
                        height: 1
                        color: node.look.kitSeparator
                    }
                }
            }

            // A group is a stacked card (docs/canvas-look.md 9, 10): a second card 1u down and right
            // behind it, so it reads as "holds nodes" and never as a Frame.
            Rectangle {
                visible: node.groupSize > 0
                x: face.u
                y: face.u
                width: parent.width
                height: parent.height
                radius: face.corner
                color: face.bodyFill
                border.width: 1
                border.color: node.look.hairline
            }
            // The box: body fill and a 1 px edge (dashed for bypassed and missing nodes, below).
            // The fill has a sheen (#72): a little lighter at the top, as if lit from above, and
            // lit more while hovered or selected; the rectangle's own two-stop gradient, drawn in
            // its vertex colors, so it costs nothing.
            Rectangle {
                id: box
                readonly property bool lit: node.hovered || node.selected
                anchors.fill: parent
                radius: face.corner
                color: face.compact ? face.bodyFill : "transparent"
                gradient: face.compact ? null : sheen
                property Gradient sheen: Gradient {
                    GradientStop {
                        position: 0
                        color: Qt.tint(face.bodyFill, box.lit ? node.look.litTop : node.look.sheenTop)
                        Behavior on color { ColorAnimation { duration: 120; easing.type: Easing.OutCubic } }
                    }
                    GradientStop {
                        position: 1
                        color: Qt.tint(face.bodyFill, box.lit ? node.look.litBottom : node.look.sheenBottom)
                        Behavior on color { ColorAnimation { duration: 120; easing.type: Easing.OutCubic } }
                    }
                }
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
                // A running node's header takes a faint Dusk tint, lilac at the left to rose at the right (the board's two radials); Compact only.
                gradient: face.compact && face.running ? runTint : null
                property Gradient runTint: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: Qt.alpha(node.look.tone("dusk").blobs[0], 0.35) }
                    GradientStop { position: 0.5; color: Qt.alpha(node.look.tone("dusk").blobs[0], 0) }
                    GradientStop { position: 0.5; color: Qt.alpha(node.look.tone("dusk").blobs[1], 0) }
                    GradientStop { position: 1; color: Qt.alpha(node.look.tone("dusk").blobs[1], 0.3) }
                }

                // Dark themes: a 1 px highlight along the top.
                Rectangle {
                    visible: !node.look.light && face.full && !face.compact
                    x: Math.max(0, face.corner - 1)
                    width: parent.width - 2 * x
                    height: 1
                    color: node.look.bevel
                }
                // The kind mark, 8 px.
                Rectangle {
                    id: kind
                    readonly property string shape: face.kindPort ? face.kindPort.shape : ""
                    visible: face.kindPort !== null && node.groupSize === 0
                    x: face.compact ? face.padX - 1 : 3 * face.u - 1
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
                    id: titleText
                    // Hidden while it is edited in place (#74).
                    visible: face.full && !face.renaming
                    // A group's kind mark is the layers glyph (drawn with the decor), 12 px.
                    x: node.groupSize > 0 ? kind.x + 12 + 1.5 * face.u
                     : kind.visible ? kind.x + kind.width + (face.compact ? 7 : 1.5 * face.u) : kind.x
                    width: parent.width - x - (decor.item ? decor.metaWidth + 1.5 * face.u : 0) - (face.compact && face.full ? 14 + face.u : 0) - 2.5 * face.u
                    anchors.verticalCenter: parent.verticalCenter
                    text: node.title
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    color: node.missing ? node.look.error : node.look.text
                    font.pixelSize: face.titlePx
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

                    // Hover help: the label in bold and the manifest's description (empty: none).
                    HoverHandler { id: outputHover }
                    ToolTip.visible: (outputHover.hovered || under) && !face.dragging && face.full
                                     && (modelData.description || "") !== ""
                    ToolTip.delay: node.look.tooltipDelay
                    ToolTip.text: node.portTip(modelData)

                    Text {
                        id: outputLabel
                        visible: face.full
                        anchors.right: parent.right
                        anchors.rightMargin: face.padX
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width - 2 * face.padX - (chip.item ? (chip.item as Item).width + 2 * face.u : 0))
                        text: output.modelData.label
                        elide: Text.ElideLeft
                        textFormat: Text.PlainText
                        color: node.look.label
                        font.pixelSize: face.labelPx
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
                        objectName: "outputDot-" + output.modelData.name
                        visible: node.tier <= 1
                        x: node.width - width / 2
                        y: node.canvasModel.rowHeight / 2 - height / 2
                        shape: output.modelData.shape
                        mask: output.modelData.role === "mask"
                        fill: node.canvasView.portColor(output.modelData.color)
                        body: face.compact ? node.look.canvas : node.look.body
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
                    // A folded Advanced input has no row (6.1, 5.4).
                    readonly property bool shown: !modelData.hidden
                    y: modelData.rowTop
                    width: node.width
                    height: modelData.rowHeight
                    opacity: dim ? 0.25 : 1

                    // Hover help: the label in bold and the manifest's description (empty: none).
                    HoverHandler { id: inputHover }
                    ToolTip.visible: (inputHover.hovered || under) && !face.dragging && face.full && shown
                                     && (modelData.description || "") !== ""
                    ToolTip.delay: node.look.tooltipDelay
                    ToolTip.text: node.portTip(modelData)

                    Loader {
                        objectName: "widgetLoader"
                        active: input.hasWidget && input.shown
                        visible: face.full
                        // Under the label, except while typed into.
                        z: item && (item as InputWidget).editingText ? 1 : -1
                        // Flat: across the row, the label drawn over its well. Compact: the control right of the label, 20 px high in a
                        // row (a toggle's pill at the end of it, a multiline well across the row under its caption).
                        readonly property bool pill: input.modelData.widgetType === "toggle"
                        readonly property bool inline: face.compact && !input.tall
                        readonly property real side: face.compact ? face.padX : 3 * face.u
                        x: !inline ? side : pill ? node.width - face.padX - 32 : face.splitX
                        y: inline ? Math.round((input.height - height) / 2) : Math.round(0.5 * face.u)
                        width: !inline ? node.width - 2 * side : pill ? 32 : node.width - face.padX - face.splitX
                        height: inline ? 20 : input.height - 2 * Math.round(0.5 * face.u)
                        sourceComponent: InputWidget {
                            compact: face.compact
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
                        // The label; on a tall (multiline) well a caption line above its text, there
                        // whether it is filled or not (#74).
                        objectName: "inputLabel"
                        visible: face.full && input.shown
                        x: face.compact ? face.padX + (input.tall ? 2 * face.u : 0) : input.showsWidget ? 5 * face.u : 3 * face.u
                        y: input.tall ? Math.round(2 * face.u) : 0
                        width: face.compact ? (input.tall ? node.width - 2 * face.padX - 4 * face.u : face.splitX - face.padX - face.u)
                               : (input.hasWidget ? 0.45 : 1) * (node.width - 6 * face.u) - (input.showsWidget ? 2 * face.u : 0)
                        height: input.tall ? 16 : node.canvasModel.rowHeight
                        verticalAlignment: Text.AlignVCenter
                        text: visible ? input.modelData.label : ""
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        color: node.look.label
                        font.pixelSize: face.labelPx
                    }
                    // An unlinked widget input shows its mark only on hover (of the node, or of the
                    // port itself, which reaches past the node's edge) or to a compatible link.
                    PortDot {
                        objectName: "inputDot-" + input.modelData.name
                        visible: input.shown && node.tier <= 1
                                 && (!input.showsWidget || node.hovered || input.under || (face.dragging && !input.dim))
                        x: -width / 2
                        y: node.canvasModel.rowHeight / 2 - height / 2
                        shape: input.modelData.shape
                        mask: input.modelData.role === "mask"
                        fill: node.canvasView.portColor(input.modelData.color)
                        body: face.compact ? node.look.canvas : node.look.body
                        corner: face.corner
                        list: input.modelData.list
                        wired: input.modelData.wired
                        hovered: input.under && !face.dragging
                        hot: input.under && face.dragging && !input.dim
                    }
                }
            }

            // The last image: thumbnail (docs/canvas-look.md 4.6), in every tier.
            // Compare Images (#62): its viewer takes the slot while the node is read at full detail
            // (made only then, as the thumbnails are); zoomed out, A's thumbnail stands in.
            readonly property bool comparing: node.compare.a !== undefined
            // Crop (#73): the picture arriving at its input, with the frame over it, at full detail.
            readonly property bool cropping: node.canvasModel.isCrop(node.nodeId) && node.thumbnailText === ""
            // A node with a painted mask (Load Image, Paint Mask, #174): its picture with the mask over it, at full detail.
            readonly property var maskPort: node.inputs.find(p => p.widgetType === "mask")
            readonly property string maskFile: maskPort && !maskPort.wired && typeof maskPort.literal === "string" && App.runs
                                               ? App.runs.inputFile(maskPort.literal) : ""
            readonly property bool masked: maskFile !== "" && node.thumbnailText === ""
            // A video or a sound result (Save Video, Save Audio, Preview): a player in the slot at full detail.
            readonly property bool playable: node.media.url !== undefined
            // One Loader, so a node costs the one item it always did (#5).
            Loader {
                id: pictureLoader
                active: node.thumbnail !== "" && !node.collapsed
                x: node.thumbnailRect.x
                y: node.thumbnailRect.y
                width: node.thumbnailRect.width
                height: node.thumbnailRect.height
                objectName: "thumbnail"
                sourceComponent: !face.full ? thumbnailComponent : face.comparing ? compareComponent
                                 : face.cropping ? cropComponent : face.masked ? maskComponent
                                 : face.playable ? mediaComponent : thumbnailComponent
                onItemChanged: node.compareItem = face.comparing && face.full ? item : null
                Component {
                    id: thumbnailComponent
                    Thumbnail {
                        source: node.thumbnail
                        info: node.thumbnailInfo
                        file: node.thumbnailFile
                        text: node.thumbnailText
                        look: node.look
                        full: face.full
                        compact: face.compact
                        // A placeholder, and the last run's picture while the node runs again (not
                        // this run's preview), are at half strength.
                        opacity: node.thumbnailDim ? 0.5 : 1
                    }
                }
                Component {
                    id: cropComponent
                    CropNodeView {
                        objectName: "cropViewer"
                        canvasModel: node.canvasModel
                        nodeId: node.nodeId
                        look: node.look
                        source: node.thumbnail
                        wheelModifiers: Qt.AltModifier // a plain wheel moves the canvas
                        onPressed: node.canvasModel.select([node.nodeId])
                        onActivated: node.canvasView.openCrop(node.nodeId)
                        opacity: node.thumbnailDim ? 0.5 : 1
                    }
                }
                Component {
                    id: maskComponent
                    MaskNodeView {
                        objectName: "maskViewer"
                        canvasModel: node.canvasModel
                        nodeId: node.nodeId
                        look: node.look
                        source: node.thumbnail
                        info: node.thumbnailInfo
                        text: node.thumbnailText
                        maskFile: face.maskFile
                        opacity: node.thumbnailDim ? 0.5 : 1
                    }
                }
                Component {
                    id: mediaComponent
                    MediaView {
                        objectName: "mediaPlayer"
                        look: node.look
                        kind: node.media.type
                        label: node.media.file || node.media.label || ""
                        // The file is opened only while the node is in view: a decoder per video is held from then on.
                        source: node.inView ? node.media.url : ""
                        expandable: node.media.type === "video"
                        onPressed: node.canvasModel.select([node.nodeId])
                        onActivated: node.canvasView.openMedia(node.nodeId)
                        onExpandRequested: node.canvasView.openMedia(node.nodeId)
                        opacity: node.thumbnailDim ? 0.5 : 1
                    }
                }
                Component {
                    id: compareComponent
                    CompareNodeView {
                        objectName: "compareViewer"
                        canvasModel: node.canvasModel
                        nodeId: node.nodeId
                        compare: node.compare
                        viewState: node.compareView
                        look: node.look
                        // Selected: the wheel zooms the picture, with or without a key; not selected, a plain wheel moves the canvas (#437)
                        wheelModifiers: node.selected ? Qt.KeyboardModifierMask : Qt.AltModifier
                        zoomChips: true // Fit and 100% on hover (#420)
                        onPressed: {
                            node.canvasModel.select([node.nodeId])
                            node.canvasView.forceActiveFocus() // the viewer took the focus; the Graph's keys are the node's
                        }
                        onActivated: node.canvasView.openCompare(node.nodeId)
                        opacity: node.thumbnailDim ? 0.5 : 1
                    }
                }
            }
            // The resize marks (#49, #63): every corner resizes, and all four show their mark while
            // the node is hovered or selected (as ComfyUI's Vue nodes), in `label`, which reads at
            // 4.5:1 on the body and the header; the corner under the pointer or being dragged is
            // in `text`. FugueCanvas takes the press and the double click.
            Loader {
                active: face.full && !node.collapsed && (node.hovered || node.selected || node.activeCorner !== "")
                anchors.fill: parent
                sourceComponent: Item {
                    Repeater {
                        model: ["nw", "ne", "sw", "se"]
                        ResizeMark {
                            required property string modelData
                            corner: modelData
                            nodeCorner: face.corner
                            unit: face.u
                            color: modelData === node.activeCorner ? node.look.text : node.look.label
                            x: modelData.endsWith("w") ? 0 : node.width - width
                            y: modelData.startsWith("n") ? 0 : node.height - height
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
                        || face.dashed || node.collapsed || extrasNeeded || node.groupSize > 0 || face.renaming
                // What only some nodes have, made here too so a big graph pays nothing for it
                // (test_canvas_window counts items): section headers (6.1, 5.4; the Advanced one
                // opens and folds with a click, which FugueCanvas takes), the names an entered
                // group's face gives its inner ports (5.4), and the update chip of a group the library
                // has a newer version of (5.3), or the chip of a node that has a newer major (3.6, #118).
                readonly property bool extrasNeeded: face.full && node.rowsMade && !node.collapsed
                    && (node.updateAvailable !== "" || node.newerMajor !== ""
                        || node.inputs.some(p => p.headerTop !== undefined || p.exposedAs !== undefined)
                        || node.outputs.some(p => p.exposedAs !== undefined))
                anchors.fill: parent
                // The width its meta text takes from the title, set by the item while it is there.
                property real metaWidth: 0
                sourceComponent: Item {
                    Binding {
                        decor.metaWidth: meta.visible ? meta.width : 0
                    }
                    // The title edited in place (F2, a double-click on it, Rename on the menu; #74),
                    // over the header's title.
                    Loader {
                        active: face.renaming
                        x: 1 + titleText.x
                        width: titleText.width
                        y: 1 + ((node.collapsed ? node.height - 2 : node.canvasModel.headerHeight - 1) - height) / 2
                        sourceComponent: TitleEditor {
                            initial: node.title
                            color: node.look.text
                            font.pixelSize: node.look.textSize
                            font.weight: Font.DemiBold
                            onDone: keep => {
                                if (keep)
                                    node.canvasModel.renameNode(node.nodeId, text)
                                node.canvasView.renamingNode = ""
                            }
                        }
                    }
                    // A group card's footer: how many nodes it holds, and a button that enters it (as
                    // ComfyUI's "Enter subgraph", #74; Tab and a double-click on the body too).
                    Loader {
                        active: node.groupSize > 0 && face.full && !node.collapsed
                        x: 3 * face.u
                        y: node.height - 2 * face.u - node.canvasModel.rowHeight
                        width: node.width - 6 * face.u
                        height: node.canvasModel.rowHeight
                        sourceComponent: Item {
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - enterButton.width - face.u
                                text: qsTr("%n nodes", "", node.groupSize)
                                elide: Text.ElideRight
                                color: node.look.label
                                font.pixelSize: node.look.captionSize
                            }
                            Rectangle {
                                id: enterButton
                                objectName: "enterGroupButton"
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width: enterText.implicitWidth + 2 * face.u
                                height: parent.height - face.u
                                radius: node.look.smallCorner
                                color: enterArea.containsMouse ? node.look.well : "transparent"
                                Text {
                                    id: enterText
                                    anchors.centerIn: parent
                                    text: qsTr("Enter group") + " \u203a"
                                    color: enterArea.containsMouse ? node.look.text : node.look.label
                                    font.pixelSize: node.look.captionSize
                                }
                                MouseArea {
                                    id: enterArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: node.canvasModel.groups.enterGroup(node.nodeId)
                                }
                            }
                        }
                    }

                    Repeater {
                        model: decor.extrasNeeded ? node.inputs.filter(p => p.headerTop !== undefined) : []
                        delegate: Row {
                            id: header
                            required property var modelData
                            objectName: "sectionHeader"
                            readonly property bool advanced: modelData.advancedHeader === true
                            x: face.padX
                            y: modelData.headerTop
                            height: node.canvasModel.rowHeight
                            spacing: face.u
                            Glyph {
                                visible: header.advanced
                                anchors.verticalCenter: parent.verticalCenter
                                look: node.look
                                name: header.modelData.advancedOpen ? "chevron-down" : "chevron-right"
                                color: node.look.label
                                font.pixelSize: 12
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.min(implicitWidth, node.width - 6 * face.u - (header.advanced ? 12 + face.u : 0))
                                text: !header.advanced ? header.modelData.header
                                    : header.modelData.advancedOpen ? qsTr("Advanced")
                                    : qsTr("Advanced (%1)").arg(header.modelData.folded)
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                color: node.look.label
                                font.pixelSize: node.look.captionSize
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                    // Compact: a hairline above a section's header, inset by the card's padding.
                    Repeater {
                        model: decor.extrasNeeded && face.hairlines ? node.inputs.filter(p => p.headerTop !== undefined && p.headerTop > node.canvasModel.headerHeight + 2 * face.u) : []
                        delegate: Rectangle {
                            required property var modelData
                            x: face.padX
                            y: modelData.headerTop
                            width: node.width - 2 * face.padX
                            height: 1
                            color: node.look.kitSeparator
                        }
                    }
                    Repeater {
                        model: !decor.extrasNeeded ? [] : node.inputs.filter(p => p.exposedAs !== undefined && !p.hidden)
                            .concat(node.outputs.filter(p => p.exposedAs !== undefined))
                        delegate: Text {
                            required property var modelData
                            objectName: "exposedMark"
                            x: modelData.output ? node.width + 2 * face.u : -implicitWidth - 2 * face.u
                            y: modelData.rowTop + (node.canvasModel.rowHeight - height) / 2
                            text: modelData.output ? "\u2192 " + modelData.exposedLabel
                                                   : modelData.exposedLabel + " \u2192"
                            textFormat: Text.PlainText
                            color: node.look.accent
                            font.pixelSize: node.look.captionSize
                        }
                    }
                    Rectangle {
                        objectName: "updateChip"
                        visible: decor.extrasNeeded && (node.updateAvailable !== "" || node.newerMajor !== "")
                        x: node.width - width
                        y: -height - Math.round(face.u)
                        width: updateText.implicitWidth + 3 * face.u
                        height: updateText.implicitHeight + face.u
                        radius: node.look.smallCorner
                        color: node.look.accent
                        Text {
                            id: updateText
                            anchors.centerIn: parent
                            text: !parent.visible ? ""
                                  : node.updateAvailable !== "" ? qsTr("Update to %1").arg(node.updateAvailable)
                                  : qsTr("Move to %1").arg(node.newerMajor)
                            textFormat: Text.PlainText
                            color: node.look.accentForeground
                            font.pixelSize: node.look.captionSize
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                node.canvasModel.select([node.nodeId], "replace")
                                if (node.updateAvailable !== "")
                                    node.canvasModel.groups.requestGroupUpdate()
                                else
                                    node.canvasModel.requestMajorMove()
                            }
                        }
                    }

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
                            strokeColor: node.missing ? node.look.error : node.look.bypassEdge
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
                        x: node.width - width - (face.compact && face.full ? face.padX + 14 + face.u : 2.5 * face.u)
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
                        // Warnings: a count and the triangle, in the label color. Hovered, they
                        // show the warnings; a click keeps them up (#72; the canvas's one card).
                        Loader {
                            // Made only while the node has warnings: no items for the others.
                            active: node.warnings.length > 0
                            visible: active
                            anchors.verticalCenter: parent.verticalCenter
                            sourceComponent: Item {
                                id: warningsBadge
                                objectName: "warningsBadge"
                                width: warningsRow.implicitWidth
                                height: warningsRow.implicitHeight
                                // Reachable by the keyboard: Enter or Space keeps the card up, with the focus on its first button.
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: qsTr("Warnings: %1").arg(node.warnings.length)
                                Keys.onReturnPressed: event => { node.canvasView.showWarnings(node.nodeId, node.warnings, warningsBadge, true); node.canvasView.focusWarnings(); event.accepted = true }
                                Keys.onEnterPressed: event => { node.canvasView.showWarnings(node.nodeId, node.warnings, warningsBadge, true); node.canvasView.focusWarnings(); event.accepted = true }
                                Keys.onSpacePressed: event => { node.canvasView.showWarnings(node.nodeId, node.warnings, warningsBadge, true); node.canvasView.focusWarnings(); event.accepted = true }
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -2
                                    visible: warningsBadge.activeFocus && FocusInput.keyboard
                                    color: "transparent"
                                    radius: 3
                                    border.width: 2
                                    border.color: node.look.accent
                                }
                                Row {
                                    id: warningsRow
                                    spacing: face.u
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: node.warnings.length
                                        color: node.look.label
                                        font.pixelSize: node.look.captionSize
                                    }
                                    Glyph {
                                        anchors.verticalCenter: parent.verticalCenter
                                        name: "triangle-alert"
                                        look: node.look
                                        color: node.look.label
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    function show(pin) {
                                        node.canvasView.showWarnings(node.nodeId, node.warnings, warningsBadge, pin)
                                    }
                                    onEntered: show(false)
                                    onExited: node.canvasView.hideWarnings()
                                    onClicked: show(true)
                                }
                            }
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

                    // A group: the layers glyph as its kind mark, and how many nodes it holds.
                    Glyph {
                        visible: node.groupSize > 0
                        x: 3 * face.u - 1
                        y: (node.canvasModel.headerHeight - height) / 2
                        look: node.look
                        name: "layers"
                        color: node.look.label
                        font.pixelSize: 12
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

                    // Routed to a service off this machine (8.2, #404): a quiet mark above the node, so what a run sends
                    // there is never a surprise. It clears no other chip: those are under the node.
                    Text {
                        objectName: "runsOnMark"
                        visible: node.runsOn !== "" && face.full
                        x: 0
                        y: -height - Math.round(0.5 * face.u)
                        width: node.width
                        text: qsTr("Runs on %1").arg(node.runsOn)
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: node.look.label
                        font.pixelSize: node.look.captionSize
                        Accessible.name: text
                    }

                    // Cannot run now (8.5): what is missing and how to fix it, in a chip under the
                    // node in the label color, until a run says more.
                    Rectangle {
                        objectName: "noticeChip"
                        visible: !face.failed && node.notice !== "" && face.full
                        y: node.height + Math.round(1.5 * face.u)
                        width: node.width
                        height: noticeText.height + 3 * face.u
                        radius: node.look.smallCorner
                        color: node.look.well
                        Rectangle {
                            width: 2
                            height: parent.height
                            color: node.look.label
                        }
                        Glyph {
                            x: 2 * face.u
                            y: 1.5 * face.u
                            name: "triangle-alert"
                            look: node.look
                            color: node.look.label
                        }
                        Text {
                            id: noticeText
                            x: 2 * face.u + 14 + 1.5 * face.u
                            y: 1.5 * face.u
                            width: parent.width - x - 2 * face.u
                            text: node.notice
                            wrapMode: Text.Wrap
                            maximumLineCount: 4
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            lineHeightMode: Text.FixedHeight
                            lineHeight: 15
                            color: node.look.text
                            font.pixelSize: node.look.labelSize
                        }
                    }

                    // In error: the message in a chip under the node (4.7). A click lists the
                    // run's errors and brings this node to the middle of the view.
                    Rectangle {
                        objectName: "errorChip"
                        visible: face.failed && node.error !== "" && face.full
                        y: node.height + Math.round(1.5 * face.u)
                        width: node.width
                        height: errorText.height + 3 * face.u + (installPackChip.visible ? installPackChip.height + face.u : 0)
                        radius: node.look.smallCorner
                        color: node.look.errorChip
                        // The full text (a missing pack's words are short; the technical text is here).
                        HoverHandler { id: errorHover }
                        ToolTip.visible: errorHover.hovered && node.error !== ""
                        ToolTip.delay: node.look.tooltipDelay
                        ToolTip.text: node.errorDetail !== "" ? node.errorDetail : node.error
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: node.canvasView.showNodeError(node.nodeId)
                        }
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
                            maximumLineCount: 4
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            lineHeightMode: Text.FixedHeight
                            lineHeight: 15
                            color: node.look.text
                            font.pixelSize: node.look.labelSize
                        }
                        // A node of a pack that is not installed: the button of the banner's flow (#91,
                        // Settings > Packs, which asks before it installs and before it trusts).
                        Rectangle {
                            id: installPackChip
                            objectName: "installPackChip"
                            visible: node.errorPack.installable === true
                            x: errorText.x
                            y: errorText.y + errorText.height + face.u
                            width: installPackText.implicitWidth + 4 * face.u
                            height: 7 * face.u
                            radius: node.look.smallCorner
                            color: node.look.accent
                            Text {
                                id: installPackText
                                anchors.centerIn: parent
                                text: qsTr("Install…")
                                textFormat: Text.PlainText
                                color: node.look.accentForeground
                                font.pixelSize: node.look.captionSize
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: App.workflows.installPackById(node.errorPack.id)
                            }
                        }
                    }
                }
            }
        }
    }
}
