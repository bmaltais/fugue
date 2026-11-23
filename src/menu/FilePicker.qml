pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import Fugue

// The one file (or folder) picker of the window, with FileDialog's QML surface (title, fileMode,
// currentFolder, nameFilters, defaultSuffix, selectedFile, selectedFiles, open(), close(),
// accepted, rejected) so a call site reads as before, and `suggestedName` for a save.
//
// It asks the desktop portal (PortalFileDialog: org.freedesktop.portal.FileChooser), the picker
// the desktop's own file-chooser backend draws. Qt's FileDialog is no use for that on Omarchy: the
// platform theme (qtengine) has no native file dialog, so it would always show its QML one. Qt's
// dialog is the fallback, where the portal is not on the bus (`usePortal` false) or its call
// failed, so a picker always opens. The fallback is an item of the window (popupType Popup.Item,
// not a window of its own with a system title bar, found through this object's parent), so it takes the window's Material look, set
// from App.theme in Main.qml, and its text reads on its surface.
//
// `fileMode` is PortalFileDialog.OpenFile, OpenFiles, SaveFile or Directory (the numbers of
// FileDialog's, then the folder); in Directory mode the answer is `selectedFolder`.
QtObject {
    id: root
    // QtObject is no item, so it can be made under a Popup (as FileDialog could); the dialogs below find the window through it.

    property string title
    property int fileMode: PortalFileDialog.OpenFile
    property url currentFolder
    property var nameFilters: []
    property string defaultSuffix
    // The name a save starts with (else the name of `selectedFile`, as FileDialog does).
    property string suggestedName
    property url selectedFile
    property list<url> selectedFiles
    property url selectedFolder
    // Whether the portal is asked first. A test, or a harness with no portal, sets it false.
    property bool usePortal: portal.available
    // Which dialog the last open() went to: "portal" or "qt" ("" before the first).
    property string route
    readonly property bool folder: fileMode === PortalFileDialog.Directory

    signal accepted
    signal rejected

    function open() {
        root.route = ""
        if (root.usePortal) {
            portal.title = root.title
            portal.fileMode = root.fileMode
            portal.currentFolder = root.currentFolder
            portal.nameFilters = root.nameFilters
            portal.defaultSuffix = root.defaultSuffix
            portal.suggestedName = root.suggestedName
            portal.selectedFile = root.selectedFile
            root.route = "portal"
            portal.open() // a refusal comes back as portal.onFailed, which opens Qt's dialog
        } else {
            root.openQt()
        }
    }

    function close() {
        portal.close()
        qtFile.close()
        qtFolder.close()
    }

    Component.onDestruction: root.close()

    // The functions the dialogs and open() share.
    function openQt() {
        root.route = "qt"
        if (root.folder) {
            qtFolder.title = root.title
            qtFolder.currentFolder = root.currentFolder
            qtFolder.open()
            return
        }
        qtFile.title = root.title
        qtFile.fileMode = root.fileMode
        qtFile.currentFolder = root.currentFolder
        qtFile.nameFilters = root.nameFilters
        qtFile.defaultSuffix = root.defaultSuffix
        if (root.selectedFile.toString() !== "")
            qtFile.selectedFile = root.selectedFile
        else if (root.suggestedName !== "" && root.currentFolder.toString() !== "")
            qtFile.selectedFile = root.currentFolder + "/" + root.suggestedName
        qtFile.open()
    }

    function take(file, files, folder) {
        root.selectedFile = file
        root.selectedFiles = files
        root.selectedFolder = folder
        root.accepted()
    }

    // The three dialogs are properties, as a QtObject has no default property to hold children.
    readonly property PortalFileDialog portalPart: PortalFileDialog {
        id: portal
        onAccepted: root.take(portal.selectedFile, portal.selectedFiles, portal.selectedFile)
        onRejected: root.rejected()
        onFailed: reason => {
            console.warn("FilePicker: the file chooser portal failed (" + reason + "), using Qt's dialog")
            root.openQt()
        }
    }

    readonly property FileDialog qtFilePart: FileDialog {
        id: qtFile
        popupType: Popup.Item
        onAccepted: root.take(qtFile.selectedFile, qtFile.selectedFiles, qtFile.selectedFile)
        onRejected: root.rejected()
    }

    readonly property FolderDialog qtFolderPart: FolderDialog {
        id: qtFolder
        popupType: Popup.Item
        onAccepted: root.take(qtFolder.selectedFolder, [qtFolder.selectedFolder], qtFolder.selectedFolder)
        onRejected: root.rejected()
    }
}
