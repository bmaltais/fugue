pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue

// The three questions of the Canvas view's File menu (#232 slice B): Rename canvas…, Clear canvas… and Delete canvas…. Each
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
    function askClear() { clearAsk.open() }
    function askDelete() { deleteAsk.open() }

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
