pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// The layers of the paint window (#181, spec 14.4): a list of a PaintLayer's layers, top first, each with
// an eye (visible or not), a kind glyph and its name; the picture's placeholder is a row like any other,
// saying whether a picture is there. A press selects the layer (the one that takes paint), a double click
// on the name renames it. Under the list: the active layer's opacity, then add a paint layer, add a mask
// layer, remove, move up and move down. It knows nothing of the window: give it `painting` (the PaintLayer),
// and it calls the layer's own layer calls. Which layers there are, and the undo of adding, removing and
// moving them, are the layer's.
//
// Every control is 28px tall (7u), above the 24px floor; the rows 32px. Tab reaches the buttons, and
// Up and Down move the selection in the list.
Rectangle {
    id: root
    objectName: "paintLayersPanel"
    required property CanvasLook look
    required property var painting // a PaintLayer
    readonly property var entries: painting.layers
    readonly property var active: entries.find(e => e.active) ?? null
    readonly property bool removable: active !== null && active.kind !== "picture"
    readonly property int activeIndex: entries.findIndex(e => e.active)

    width: 48 * look.unit
    radius: look.smallCorner
    color: look.well

    Column {
        anchors.fill: parent
        anchors.margins: root.look.unit
        spacing: root.look.unit

        Text {
            objectName: "paintLayersTitle"
            text: qsTr("Layers")
            color: root.look.label
            font.pixelSize: root.look.captionSize
            leftPadding: root.look.unit
            height: 5 * root.look.unit
            verticalAlignment: Text.AlignVCenter
        }

        ListView {
            id: list
            objectName: "paintLayerList"
            width: parent.width
            height: parent.height - y - controls.height - parent.spacing
            clip: true
            model: root.entries
            spacing: root.look.unit / 2
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: true
            Keys.onPressed: event => {
                const at = root.activeIndex
                if (event.key === Qt.Key_Up && at > 0) root.painting.selectLayer(root.entries[at - 1].id)
                else if (event.key === Qt.Key_Down && at >= 0 && at < root.entries.length - 1) root.painting.selectLayer(root.entries[at + 1].id)
                else return
                event.accepted = true
            }
            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property bool isPicture: modelData.kind === "picture"
                objectName: "paintLayer-" + index
                width: list.width
                height: 8 * root.look.unit
                radius: root.look.smallCorner
                color: modelData.active ? root.look.selectedRow : "transparent"
                border.width: modelData.active && list.activeFocus ? 2 : 0
                border.color: root.look.accent
                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.name
                Accessible.selected: modelData.active

                TapHandler {
                    onTapped: root.painting.selectLayer(row.modelData.id)
                }
                // The eye: a hidden layer is dimmed, as the toolbar's own toggles are.
                ViewButton {
                    id: eye
                    objectName: "paintLayerEye-" + row.index
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: root.look.unit / 2
                    look: root.look
                    glyph: "eye"
                    tip: row.modelData.visible ? qsTr("Hide the layer") : qsTr("Show the layer")
                    current: row.modelData.visible
                    accented: false
                    opacity: row.modelData.visible ? 1 : 0.55
                    onTapped: root.painting.setLayerVisible(row.modelData.id, !row.modelData.visible)
                }
                Text {
                    id: kind
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: eye.right
                    anchors.leftMargin: root.look.unit
                    font.family: root.look.iconFont
                    font.pixelSize: 14
                    color: root.look.label
                    text: root.look.glyph(row.modelData.kind === "mask" ? "square-dashed" : row.isPicture ? "image" : "brush")
                    opacity: row.modelData.visible ? 1 : 0.55
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: kind.right
                    anchors.leftMargin: root.look.unit
                    anchors.right: parent.right
                    anchors.rightMargin: root.look.unit
                    opacity: row.modelData.visible ? 1 : 0.55
                    Text {
                        objectName: "paintLayerName-" + row.index
                        width: parent.width
                        visible: !nameEdit.visible
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: row.modelData.name
                        color: root.look.text
                        font.pixelSize: root.look.labelSize
                        TapHandler {
                            acceptedButtons: Qt.LeftButton
                            onDoubleTapped: {
                                nameEdit.text = row.modelData.name
                                nameEdit.visible = true
                                nameEdit.forceActiveFocus()
                                nameEdit.selectAll()
                            }
                        }
                    }
                    ThemedTextField {
                        id: nameEdit
                        objectName: "paintLayerRename"
                        visible: false
                        width: parent.width
                        height: 5 * root.look.unit
                        padding: 2
                        font.pixelSize: root.look.labelSize
                        maximumLength: 64
                        onAccepted: {
                            const text = nameEdit.text.trim()
                            if (text !== "")
                                root.painting.setLayerName(row.modelData.id, text)
                            visible = false
                        }
                        Keys.onEscapePressed: event => { visible = false; event.accepted = true }
                        onActiveFocusChanged: if (!activeFocus) visible = false
                    }
                    // The picture's slot: what stands in it.
                    Text {
                        visible: row.isPicture
                        width: parent.width
                        elide: Text.ElideRight
                        color: root.look.label
                        font.pixelSize: root.look.captionSize
                        text: root.painting.hasPicture ? qsTr("The input picture") : qsTr("No picture: a blank canvas")
                    }
                }
            }
        }

        Column {
            id: controls
            width: parent.width
            spacing: root.look.unit
            ValueSlider {
                objectName: "paintLayerOpacity"
                width: parent.width
                look: root.look
                label: qsTr("Opacity")
                enabled: root.active !== null
                from: 0
                to: 1
                step: 0.01
                value: root.active ? root.active.opacity : 1
                shown: qsTr("%1%").arg(Math.round(value * 100))
                tip: qsTr("The layer's opacity")
                onMoved: v => { if (root.active) root.painting.setLayerOpacity(root.active.id, Math.max(0, Math.min(1, v))) }
            }
            Row {
                spacing: root.look.unit
                ViewButton {
                    objectName: "paintLayerAdd"
                    look: root.look
                    glyph: "plus"
                    text: qsTr("Layer")
                    tip: qsTr("Add a paint layer on top")
                    onTapped: root.painting.addLayer("paint")
                }
                ViewButton {
                    objectName: "paintLayerAddMask"
                    look: root.look
                    glyph: "plus"
                    text: qsTr("Mask")
                    tip: qsTr("Add a mask layer on top")
                    onTapped: root.painting.addLayer("mask")
                }
            }
            Row {
                spacing: root.look.unit
                ViewButton {
                    objectName: "paintLayerUp"
                    look: root.look
                    glyph: "arrow-up"
                    tip: qsTr("Move the layer up")
                    enabled: root.activeIndex > 0
                    onTapped: root.painting.moveLayer(root.active.id, 1)
                }
                ViewButton {
                    objectName: "paintLayerDown"
                    look: root.look
                    glyph: "arrow-down"
                    tip: qsTr("Move the layer down")
                    enabled: root.activeIndex >= 0 && root.activeIndex < root.entries.length - 1
                    onTapped: root.painting.moveLayer(root.active.id, -1)
                }
                // The destructive one, apart by a gap of its own; undoable.
                Item { width: root.look.unit; height: 1 }
                ViewButton {
                    objectName: "paintLayerRemove"
                    look: root.look
                    glyph: "trash-2"
                    tip: root.removable ? qsTr("Remove the layer") : qsTr("The picture layer cannot be removed")
                    danger: true
                    enabled: root.removable
                    onTapped: root.painting.removeLayer(root.active.id)
                }
            }
        }
    }
}
