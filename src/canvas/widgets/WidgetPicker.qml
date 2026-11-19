pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

// What a widget opens beyond its field: the menu of a choice, seed or model widget, and the
// file, folder and color pickers (the desktop portal's, Qt's own where there is none). An
// InputWidget makes one the first time it needs it.
Item {
    id: picker
    required property InputWidget host

    // `entries` null: the host's model list, kept current while models arrive.
    function openMenu(entries) {
        const well = host
        const at = well.mapToItem(Overlay.overlay, 0, well.height)
        const end = well.mapToItem(Overlay.overlay, well.width, 0)
        const menu = menuComponent.createObject(picker, {
            x: at.x, y: at.y, width: Math.max(end.x - at.x, 120), anchorHeight: well.height,
            entries: entries !== null ? entries : host.modelEntries(), followModels: entries === null
        }) as WidgetMenu
        menu.open()
    }
    Component {
        id: menuComponent
        WidgetMenu {
            id: m
            property bool followModels: false
            look: picker.host.look
            parent: Overlay.overlay
            onPicked: value => picker.host.picked(value)
            onClosed: m.destroy()
            Connections {
                target: picker.host.edits
                enabled: m.followModels
                function onModelsChanged() { m.entries = picker.host.modelEntries() }
            }
        }
    }

    function openDialog() {
        const kind = host.kind
        if (kind === "color")
            (colorDialog.createObject(picker) as ColorDialog).open()
        else if (kind === "folder")
            (folderDialog.createObject(picker) as FolderDialog).open()
        else
            (fileDialog.createObject(picker) as FileDialog).open()
    }
    Component {
        id: fileDialog
        FileDialog {
            id: d
            objectName: "widgetFileDialog"
            title: picker.host.port.label
            fileMode: FileDialog.OpenFile
            onAccepted: { picker.host.edits.setPath(picker.host.nodeId, picker.host.name, d.selectedFile); d.destroy() }
            onRejected: d.destroy()
        }
    }
    Component {
        id: folderDialog
        FolderDialog {
            id: d
            objectName: "widgetFolderDialog"
            title: picker.host.port.label
            onAccepted: { picker.host.edits.setPath(picker.host.nodeId, picker.host.name, d.selectedFolder); d.destroy() }
            onRejected: d.destroy()
        }
    }
    Component {
        id: colorDialog
        ColorDialog {
            id: d
            objectName: "widgetColorDialog"
            title: picker.host.port.label
            options: ColorDialog.ShowAlphaChannel
            selectedColor: picker.host.colorOf(picker.host.valueText, picker.host.look.text)
            onAccepted: { picker.host.edits.setColor(picker.host.nodeId, picker.host.name, d.selectedColor); d.destroy() }
            onRejected: d.destroy()
        }
    }
}
