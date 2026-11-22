pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Fugue

// New canvas… (#211; its own component since #228, the cohesion audit's C7): a name and a size, or a picture file to
// make it from, then Cancel and the one highlighted Create (Enter in any field). It makes nothing itself: `make` does,
// so the canvas picker puts the new id in its input and a Canvas view opens the canvas. The reason a canvas is refused
// shows in the error color and the question stays open to correct it. Made on each use; it destroys itself on close.
Question {
    id: ask
    objectName: "newCanvasAsk"
    // (name, width, height, picture URL or "") → why not, or "" once the canvas is made.
    required property var make
    // (picture URL) → its size as "300 × 120", or "" when it cannot be read.
    required property var sizeOf
    // A picture file URL when the canvas is made from one, else empty (a blank canvas).
    property string picture: ""
    property string problem: ""
    readonly property string pictureName: picture === "" ? "" : decodeURIComponent(picture.toString().split("/").pop())
    readonly property string pictureSize: picture === "" ? "" : sizeOf(picture)
    parent: Overlay.overlay
    onClosed: ask.destroy()
    onOpened: { nameField.forceActiveFocus(); nameField.selectAll() }
    function create() {
        problem = make(nameField.text, parseInt(widthField.text), parseInt(heightField.text), picture)
        if (problem === "")
            ask.close()
    }
    Component {
        id: pictureDialog
        FileDialog {
            id: d
            objectName: "canvasPictureDialog"
            title: qsTr("Import a picture as a canvas") // the menu's Import picture…, the CLI's fugue canvas import
            fileMode: FileDialog.OpenFile
            nameFilters: [qsTr("Pictures and canvases (*.png *.jpg *.jpeg *.webp *.ora)"), qsTr("All files (*)")]
            onAccepted: { ask.picture = d.selectedFile; d.destroy() }
            onRejected: d.destroy()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 3 * ask.look.unit
        Label { text: qsTr("New canvas"); font.bold: true; Layout.fillWidth: true }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 3 * ask.look.unit
            rowSpacing: ask.look.unit
            Label { text: qsTr("Name"); color: ask.look.label }
            ThemedTextField {
                id: nameField
                objectName: "newCanvasName"
                Layout.fillWidth: true
                text: ask.pictureName === "" ? qsTr("Untitled") : ask.pictureName.replace(/\.[^.]*$/, "")
                onAccepted: ask.create()
            }
            Label { text: qsTr("Size"); color: ask.look.label; visible: ask.picture === "" }
            RowLayout {
                visible: ask.picture === ""
                Layout.fillWidth: true
                ThemedTextField {
                    id: widthField
                    objectName: "newCanvasWidth"
                    Layout.preferredWidth: 22 * ask.look.unit // 88px at the default text size, and it follows it
                    text: "1024"
                    inputMethodHints: Qt.ImhDigitsOnly
                    onAccepted: ask.create()
                }
                Label { text: "×"; color: ask.look.label }
                ThemedTextField {
                    id: heightField
                    objectName: "newCanvasHeight"
                    Layout.preferredWidth: 22 * ask.look.unit
                    text: "1024"
                    inputMethodHints: Qt.ImhDigitsOnly
                    onAccepted: ask.create()
                }
                Label { text: qsTr("px"); color: ask.look.label }
            }
            Label { text: qsTr("Picture"); color: ask.look.label; visible: ask.picture !== "" }
            Label {
                objectName: "newCanvasPictureLine"
                visible: ask.picture !== ""
                Layout.fillWidth: true
                text: ask.pictureSize === "" ? ask.pictureName : qsTr("%1 · %2").arg(ask.pictureName).arg(ask.pictureSize)
                elide: Text.ElideMiddle
            }
        }
        ThemedButton {
            objectName: "newCanvasFromPicture"
            flat: true
            text: ask.picture === "" ? qsTr("From a picture…") : qsTr("Blank canvas instead")
            onClicked: ask.picture === "" ? (pictureDialog.createObject(ask) as FileDialog).open() : ask.picture = ""
        }
        Label {
            objectName: "newCanvasProblem"
            Layout.fillWidth: true
            visible: ask.problem !== ""
            text: ask.problem
            color: ask.look.error
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 2 * ask.look.unit
            ThemedButton { objectName: "newCanvasCancel"; text: qsTr("Cancel"); onClicked: ask.close() }
            ThemedButton {
                objectName: "newCanvasOk"
                text: qsTr("Create")
                highlighted: true
                onClicked: ask.create()
            }
        }
    }
}
