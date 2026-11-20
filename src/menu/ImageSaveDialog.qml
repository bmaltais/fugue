import QtQuick
import QtQuick.Dialogs
import Fugue

// The Save Image picker (#57): asked for by App.imageActions (node.save-image), which then
// writes the chosen file. Harnesses without imageActions get no dialog. The image's real format
// is the default suffix and the only filter, so a name typed without an extension gets it
// before the dialog asks about an existing file.
Item {
    id: root
    readonly property ImageActions actions: App.imageActions

    FileDialog {
        id: dialog
        objectName: "imageSaveDialog"
        title: qsTr("Save the image")
        fileMode: FileDialog.SaveFile
        onAccepted: if (root.actions) root.actions.saveTo(selectedFile.toString())
        onRejected: if (root.actions) root.actions.cancelSave() // gone first when the window closes
    }

    // The window going while the dialog shows (quit from the keys): it closes first, so the
    // in-window dialog is not torn down mid-show.
    Component.onDestruction: dialog.close()

    Connections {
        target: root.actions
        function onSaveRequested(folder, file, suffix) {
            dialog.defaultSuffix = suffix
            dialog.nameFilters = [qsTr("%1 images (*.%2)").arg(suffix.toUpperCase()).arg(suffix)]
            dialog.currentFolder = folder
            dialog.selectedFile = file
            dialog.open()
        }
    }
}
