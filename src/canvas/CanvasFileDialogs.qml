pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../menu"

// The questions of the Canvas view's menus: File's Rename canvas…, Share… (#249), Clear canvas… and Delete canvas… (#232 slice B), and
// Layer's Layer properties… (#252: name, opacity, blend mode and layer mask of one layer, applied as ONE revision). Each
// asks first with the safe answer highlighted and focused (docs/ui-ergonomics.md D8, D9); Delete and Clear are in the
// danger tint, after the wider gap. They act through the view's model (`view.backend`).
Item {
    id: root
    objectName: "canvasFileDialogs"
    required property var view
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property bool mirrored: Application.layoutDirection === Qt.RightToLeft
    readonly property string canvasName: view.backend.canvases.find(row => row.id === view.backend.canvasId)?.name ?? ""
    visible: false

    function askRename() { renameField.text = root.canvasName; renameAsk.open() }
    function askShare() { shareAsk.open() }
    function askClear() { clearAsk.open() }
    function askDelete() { deleteAsk.open() }
    // Layer properties… of the layer at `position`: what the dialog shows is that layer's now; Apply writes what differs.
    property var propsRow: null
    property string propsBlend: "normal"
    property bool propsMaskOn: true
    property bool propsMaskRemove: false
    property real propsOpacity: 1
    function askProperties(position) {
        const row = view.backend.layers.find(r => r.position === position)
        if (!row)
            return
        propsRow = row
        propsBlend = row.blend !== "" ? row.blend : "normal"
        propsMaskOn = row.maskEnabled
        propsMaskRemove = false
        propsOpacity = row.opacity
        propsName.text = row.name
        propsAsk.open()
    }
    function applyProperties() {
        const row = propsRow
        const changes = {}
        const name = propsName.text.trim()
        if (name !== "" && name !== row.name)
            changes.name = name
        if (Math.abs(propsOpacity - row.opacity) > 0.0005)
            changes.opacity = propsOpacity
        if (row.blend !== "" && propsBlend !== row.blend)
            changes.blend = propsBlend
        if (row.hasMask && propsMaskRemove)
            changes.removeMask = true
        else if (row.hasMask && propsMaskOn !== row.maskEnabled)
            changes.maskEnabled = propsMaskOn
        propsAsk.close()
        if (Object.keys(changes).length > 0)
            view.backend.setProperties(row.position, changes)
    }

    Question {
        id: propsAsk
        objectName: "canvasPropertiesAsk"
        onOpened: { propsName.forceActiveFocus(); propsName.selectAll() }
        ColumnLayout {
            anchors.fill: parent
            spacing: 3 * root.u
            Label { text: qsTr("Layer properties"); font.bold: true; Layout.fillWidth: true }
            Label { text: qsTr("Name"); color: root.look.label }
            ThemedTextField {
                id: propsName
                objectName: "canvasPropertiesName"
                Layout.fillWidth: true
                onAccepted: propsOk.clicked()
                Accessible.name: qsTr("Layer name")
            }
            DragNumberField {
                objectName: "canvasPropertiesOpacity"
                Layout.fillWidth: true
                look: root.look
                label: qsTr("Opacity")
                tip: qsTr("Drag to change, click to type, double-click for 100%; Ctrl snaps to 10%, Shift is finer")
                value: root.propsOpacity
                format: v => qsTr("%1%").arg(Math.round(v * 100))
                parse: text => Number(String(text).replace("%", "").trim()) / 100
                onCommitted: v => root.propsOpacity = v
            }
            Label { text: qsTr("Blend mode"); color: root.look.label; visible: root.propsRow !== null && root.propsRow.blend !== "" }
            Flow {
                Layout.fillWidth: true
                spacing: root.u
                visible: root.propsRow !== null && root.propsRow.blend !== ""
                Repeater {
                    model: root.view.backend.blendModes
                    ViewButton {
                        required property var modelData
                        objectName: "canvasPropertiesBlend-" + modelData.id
                        look: root.look
                        accented: false
                        text: modelData.label
                        current: root.propsBlend === modelData.id
                        onTapped: root.propsBlend = modelData.id
                    }
                }
            }
            Label { text: qsTr("Layer mask"); color: root.look.label; visible: root.propsRow !== null && root.propsRow.hasMask }
            Row {
                spacing: root.u
                visible: root.propsRow !== null && root.propsRow.hasMask
                ViewButton {
                    objectName: "canvasPropertiesMaskOn"
                    look: root.look
                    accented: false
                    enabled: !root.propsMaskRemove
                    text: root.propsMaskOn ? qsTr("Mask on") : qsTr("Mask off")
                    checkable: true
                    current: root.propsMaskOn
                    onTapped: root.propsMaskOn = !root.propsMaskOn
                }
                ViewButton {
                    objectName: "canvasPropertiesMaskDelete"
                    look: root.look
                    danger: true
                    text: root.propsMaskRemove ? qsTr("Keep the mask") : qsTr("Delete mask")
                    onTapped: root.propsMaskRemove = !root.propsMaskRemove
                }
            }
            QuestionAnswers {
                ThemedButton { text: qsTr("Cancel"); onClicked: propsAsk.close() }
                ThemedButton {
                    id: propsOk
                    objectName: "canvasPropertiesOk"
                    text: qsTr("Apply")
                    highlighted: true
                    enabled: propsName.text.trim() !== ""
                    onClicked: root.applyProperties()
                }
            }
        }
    }

    Question {
        id: renameAsk
        objectName: "canvasRenameAsk"
        onOpened: { renameField.forceActiveFocus(); renameField.selectAll() }
        ColumnLayout {
            anchors.fill: parent
            spacing: 3 * root.u
            Label { text: qsTr("Rename the canvas") }
            ThemedTextField {
                id: renameField
                objectName: "canvasRenameField"
                Layout.fillWidth: true
                onAccepted: renameOk.clicked()
            }
            QuestionAnswers {
                ThemedButton { text: qsTr("Cancel"); onClicked: renameAsk.close() }
                ThemedButton {
                    id: renameOk
                    objectName: "canvasRenameOk"
                    text: qsTr("Rename")
                    highlighted: true
                    enabled: renameField.text.trim() !== ""
                    onClicked: { renameAsk.close(); root.view.backend.renameCanvas(renameField.text) }
                }
            }
        }
    }

    // Share… (#249): what goes in the package, then where it is saved. The safe, smaller answer (no history) is the
    // highlighted one; the file is never written over another (the model checks).
    property bool shareWithHistory: false
    Question {
        id: shareAsk
        objectName: "canvasShareAsk"
        onOpened: shareHead.forceActiveFocus()
        ColumnLayout {
            anchors.fill: parent
            spacing: 4 * root.u
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Share “%1” as a package that opens in Fugue. With its history, the person can step back through every change; without, they get the layers as they are now.").arg(root.canvasName)
            }
            QuestionAnswers {
                spacing: 4 * root.u
                ThemedButton { text: qsTr("Cancel"); onClicked: shareAsk.close() }
                ThemedButton {
                    objectName: "canvasShareWithHistory"
                    text: qsTr("With history…")
                    onClicked: { shareAsk.close(); root.shareWithHistory = true; shareDialog.openFor() }
                }
                ThemedButton {
                    id: shareHead
                    objectName: "canvasShareHead"
                    text: qsTr("As it is now…")
                    highlighted: true
                    onClicked: { shareAsk.close(); root.shareWithHistory = false; shareDialog.openFor() }
                }
            }
        }
    }
    FilePicker {
        id: shareDialog
        objectName: "canvasShareDialog"
        title: qsTr("Save the canvas as a package")
        fileMode: PortalFileDialog.SaveFile
        defaultSuffix: "fuguepack"
        nameFilters: [qsTr("Fugue packages (*.fuguepack)")]
        function openFor() {
            currentFolder = StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
            selectedFile = StandardPaths.writableLocation(StandardPaths.DocumentsLocation) + "/" + root.canvasName + ".fuguepack"
            open()
        }
        onAccepted: root.view.backend.sharePackage(selectedFile.toString(), root.shareWithHistory)
    }

    Question {
        id: clearAsk
        objectName: "canvasClearAsk"
        onOpened: clearKeep.forceActiveFocus()
        ColumnLayout {
            anchors.fill: parent
            spacing: 4 * root.u
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Clear “%1”? Every layer is replaced by one empty layer. Undo brings them back.").arg(root.canvasName)
            }
            QuestionAnswers {
                spacing: 4 * root.u
                ThemedButton {
                    id: clearKeep
                    objectName: "canvasClearKeep"
                    text: qsTr("Keep")
                    highlighted: true
                    onClicked: clearAsk.close()
                }
                ThemedButton {
                    objectName: "canvasClearYes"
                    text: qsTr("Clear")
                    danger: true
                    onClicked: { clearAsk.close(); root.view.backend.clearCanvas() }
                }
            }
        }
    }

    Question {
        id: deleteAsk
        objectName: "canvasDeleteAsk"
        onOpened: deleteKeep.forceActiveFocus()
        ColumnLayout {
            anchors.fill: parent
            spacing: 4 * root.u
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Delete “%1” for good? Its layers and its whole history are removed. This cannot be undone.").arg(root.canvasName)
            }
            QuestionAnswers {
                spacing: 4 * root.u
                ThemedButton {
                    id: deleteKeep
                    objectName: "canvasDeleteKeep"
                    text: qsTr("Keep")
                    highlighted: true
                    onClicked: deleteAsk.close()
                }
                ThemedButton {
                    objectName: "canvasDeleteYes"
                    text: qsTr("Delete")
                    danger: true
                    onClicked: { deleteAsk.close(); root.view.backend.deleteCanvas() }
                }
            }
        }
    }
}
