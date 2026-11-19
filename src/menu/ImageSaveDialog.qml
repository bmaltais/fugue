import QtQuick
import QtQuick.Dialogs
import Fugue

// The Save Image picker (#57): asked for by App.imageActions (node.save-image), which then
// writes the chosen file. Harnesses without imageActions get no dialog.
Item {
    id: root
    readonly property ImageActions actions: App.imageActions

    FileDialog {
        id: dialog
        objectName: "imageSaveDialog"
        title: qsTr("Save the image")
        fileMode: FileDialog.SaveFile
        onAccepted: root.actions.saveTo(selectedFile.toString())
        onRejected: root.actions.cancelSave()
    }

    Connections {
        target: root.actions
        function onSaveRequested(folder, name) {
            dialog.currentFolder = folder
            dialog.selectedFile = folder + "/" + name
            dialog.open()
        }
    }
}
