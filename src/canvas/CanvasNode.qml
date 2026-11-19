pragma ComponentBehavior: Bound
import QtQuick
import "widgets"

// One node on the canvas: a NodeList row (canvas_model.h). Its ports sit where the model
// says (each port's `y`), so wires and hit tests agree with what is drawn. It takes no input
// itself: FugueCanvas handles every press, so the delegate stays cheap at 300 nodes.
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

    // Set by the canvas, which works out the look once for every node.
    property var canvasModel
    property var linkDrag: ({})
    property real headerHeight: 26
    property real rowHeight: 22
    property var look
    // The world area worth drawing in detail, and whether the zoom lets text be read.
    property rect detailArea
    property bool readable: true

    // Port rows are made the first time the node is in view at a readable zoom and kept
    // after that, so a big graph opens at the cost of what is on screen (ComfyUI likewise
    // draws a node without its slots and widgets when zoomed out).
    readonly property bool inView: x < detailArea.x + detailArea.width && x + width > detailArea.x
                                   && y < detailArea.y + detailArea.height && y + height > detailArea.y
    readonly property bool wanted: inView && readable
    property bool rowsMade: false
    onWantedChanged: if (wanted) rowsMade = true
    Component.onCompleted: if (wanted) rowsMade = true

    readonly property bool dragging: linkDrag.node !== undefined
    readonly property int corner: look.corner

    property var canvasView // the FugueCanvas, for its portColor()
    function portColor(key) { return canvasView.portColor(key) }

    x: posX
    y: posY
    width: nodeWidth
    height: nodeHeight
    // A muted node fades; a bypassed one takes ComfyUI's purple (the palette's magenta).
    opacity: mode === "mute" ? 0.5 : 1

    Rectangle {
        anchors.fill: parent
        radius: node.corner
        color: node.mode === "bypass" ? Qt.tint(node.look.panel, Qt.alpha(node.look.palette.magenta, 0.35))
                                      : node.look.panel
        border.width: node.selected ? 2 : 1
        border.color: node.selected ? node.look.accent : node.missing ? node.look.error : node.look.border
    }
    Rectangle {
        width: parent.width
        height: node.headerHeight
        radius: node.corner
        color: node.look.header
        border.width: 0
        // Square the header's lower corners against the body.
        Rectangle {
            visible: !node.collapsed && node.corner > 0
            y: parent.height - node.corner
            width: parent.width
            height: node.corner
            color: parent.color
        }
        Text {
            x: 10
            width: parent.width - 20
            anchors.verticalCenter: parent.verticalCenter
            text: node.title
            elide: Text.ElideRight
            textFormat: Text.PlainText
            color: node.missing ? node.look.error : node.look.text
            font.bold: true
            font.pixelSize: node.look.textSize
        }
    }

    // One row per port: the label is the row, its mark and value hang off it.
    Repeater {
        model: node.collapsed || !node.rowsMade ? 0 : node.inputs.length
        delegate: Text {
            id: input
            required property int index
            // Read from the node's own array: a Repeater over a list of maps would convert
            // the map again on every read.
            readonly property var modelData: node.inputs[index]
            readonly property bool dim: node.dragging
                && node.canvasModel.dragProblem(node.nodeId, modelData.name, false) !== ""
            // A value input's row has its widget (widgets/InputWidget.qml) under the label.
            readonly property bool hasWidget: modelData.widget
            x: 12
            y: modelData.y - node.rowHeight / 2
            width: hasWidget ? node.width * 0.45 : node.width - 24
            height: node.rowHeight
            visible: node.readable
            opacity: dim ? 0.25 : 1
            verticalAlignment: Text.AlignVCenter
            text: modelData.label
            elide: Text.ElideRight
            textFormat: Text.PlainText
            color: node.look.muted
            font.pixelSize: node.look.smallText

            PortDot {
                x: -17
                y: (input.height - height) / 2
                shape: input.modelData.shape
                fill: node.portColor(input.modelData.color)
                edge: node.look.background
                list: input.modelData.list
            }
            Loader {
                objectName: "widgetLoader"
                active: input.hasWidget
                // Under the label, except while typed into.
                z: item && item.editingText ? 1 : -1
                y: 1
                width: node.width - 24
                height: input.height - 2
                sourceComponent: InputWidget {
                    edits: node.canvasModel.widgets
                    nodeId: node.nodeId
                    port: input.modelData
                    look: node.look
                    canvasView: node.canvasView
                    nodeItem: node
                    row: input
                }
            }
        }
    }

    Repeater {
        model: node.collapsed || !node.rowsMade ? 0 : node.outputs.length
        delegate: Text {
            id: output
            required property int index
            // Read from the node's own array: a Repeater over a list of maps would convert
            // the map again on every read.
            readonly property var modelData: node.outputs[index]
            readonly property bool dim: node.dragging
                && node.canvasModel.dragProblem(node.nodeId, modelData.name, true) !== ""
            x: node.width * 0.4 - 12
            y: modelData.y - node.rowHeight / 2
            width: node.width * 0.6
            height: node.rowHeight
            visible: node.readable
            opacity: dim ? 0.25 : 1
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignRight
            text: modelData.label
            elide: Text.ElideLeft
            textFormat: Text.PlainText
            color: node.look.muted
            font.pixelSize: node.look.smallText

            PortDot {
                x: output.width + 12 - width / 2
                y: (output.height - height) / 2
                shape: output.modelData.shape
                fill: node.portColor(output.modelData.color)
                edge: node.look.background
                list: output.modelData.list
            }
        }
    }
}
