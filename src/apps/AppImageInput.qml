pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import Fugue
import "../canvas/widgets"
import "../kit"
import "../menu"

// A picture input in ComfyUI App Mode's layout: one row with a dropdown of pictures (this template's Samples, the Recent ones
// from the inputs folder, then This app's results, so a result can go back in) and a folder button (the file picker), over a
// dashed well that shows the picture fitted. The well says "Drop, paste or browse" while empty, takes a drop, and has a small
// remove (x) button while the pointer is over it; a click focuses it (Ctrl+V pastes) and only the Browse button opens the picker. "Use sample" is in the
// dropdown whenever the template has one. AppView takes the file into the inputs folder (setImageFiles, pasteImage, useImage,
// useResultAsInput); this shows `valueOf(key)`. A refusal is one line under the well, in the error color.
ColumnLayout {
    id: root
    objectName: "appImageInput-" + field.key
    required property var field
    required property AppView appView
    required property CanvasLook look
    property string current: field.value === undefined || field.value === null ? "" : String(field.value)
    property string notice
    // Which dialog the file picker opened ("" while it has not been opened): what a test reads.
    readonly property string pickerRoute: picker.route
    property alias usePortal: picker.usePortal
    readonly property bool menuOpen: menu.opened
    property var recents: appView.recentImages()
    property var results: appView.resultPictures()
    readonly property string sample: field.sample ?? ""
    readonly property string currentName: current === "" ? "" : current.split("/").pop()
    // The dropdown's entries, in groups: the template's samples, the recent pictures they do not repeat, this app's results.
    readonly property var entries: {
        const out = []
        const seen = {}
        const add = (heading, list, prefix) => {
            const fresh = list.filter(p => seen[p.path] !== true)
            if (fresh.length === 0)
                return
            out.push({heading: true, text: heading})
            for (const p of fresh) {
                seen[p.path] = true
                out.push({text: p.name, value: prefix + p.path, thumbnail: p.url, checked: p.path === root.current})
            }
        }
        add(qsTr("Samples"), field.samples, "")
        add(qsTr("Recent"), recents.slice(0, 24), "")
        add(qsTr("This app's results"), results.slice(0, 12), "result:")
        if (root.sample !== "")
            out.push({text: qsTr("Use sample"), value: "sample:"})
        return out
    }
    spacing: look.unit / 2

    Connections {
        target: root.appView
        function onValueChanged(key) {
            if (key !== root.field.key)
                return
            root.current = String(root.appView.valueOf(key) ?? "")
            root.recents = root.appView.recentImages()
        }
        function onRunChanged() { root.results = root.appView.resultPictures() }
        // A picture the page took by Ctrl+V or a drop anywhere on it: its refusal shows here.
        function onImageNotice(key, text) {
            if (key === root.field.key)
                root.notice = text
        }
    }
    function paste() { root.notice = root.appView.pasteImage(root.field.key) }
    function remove() {
        root.appView.setValue(root.field.key, "")
        root.current = ""
    }
    function pick(value) {
        if (value === "sample:")
            root.appView.useSample(root.field.key)
        else if (value.startsWith("result:"))
            root.notice = root.appView.useResultAsInput(root.field.key, "file://" + value.substring(7))
        else
            root.appView.useImage(root.field.key, value)
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: root.look.unit
        KitDropdown {
            id: drop
            objectName: "appImageChoose-" + root.field.key
            Layout.fillWidth: true
            look: root.look
            text: root.currentName !== "" ? root.currentName : qsTr("Choose a picture")
            ink: root.currentName !== "" ? root.look.text : root.look.label
            open: menu.opened
            Accessible.name: root.field.label
            onTapped: drop.show()
            function show() {
                root.recents = root.appView.recentImages()
                root.results = root.appView.resultPictures()
                const at = drop.mapToItem(Overlay.overlay, 0, drop.height)
                menu.wantX = at.x
                menu.wantY = at.y
                menu.anchorHeight = drop.height
                menu.open()
            }
            WidgetMenu {
                id: menu
                objectName: "appImageMenu-" + root.field.key
                look: root.look
                parent: Overlay.overlay
                searchable: true
                wordStart: true
                width: Math.min(Math.max(drop.width, 36 * root.look.unit), Overlay.overlay ? Overlay.overlay.width - 4 * root.look.unit : 400)
                entries: root.entries
                onPicked: v => { root.pick(v); drop.forceActiveFocus() }
            }
        }
        KitChip {
            objectName: "appImageBrowse-" + root.field.key
            look: root.look
            pill: true
            glyph: "folder"
            tip: qsTr("Browse for a picture")
            onTapped: picker.open()
        }
    }

    Rectangle {
        id: well
        objectName: "appInput-" + root.field.key
        Layout.fillWidth: true
        implicitHeight: 40 * root.look.unit
        radius: root.look.params.cornerWell * root.look.unit
        color: "transparent"
        KitWell { look: root.look; dashed: true } // recessed; the dashed edge stays
        activeFocusOnTab: true
        // The page's Ctrl+V and drop go to the picture input last focused.
        onActiveFocusChanged: root.appView.setFocusedImage(root.field.key, activeFocus)
        Accessible.role: Accessible.Button
        Accessible.name: root.field.label
        Accessible.description: root.current === "" ? qsTr("No picture") : root.currentName
        Keys.onDeletePressed: root.remove()
        Keys.onPressed: event => {
            if (event.matches(StandardKey.Paste)) {
                root.paste()
                event.accepted = true
            }
        }
        Image {
            id: shown
            objectName: "appImageShown-" + root.field.key
            anchors { fill: parent; margins: 2 * root.look.unit }
            source: root.appView.fileUrl(root.current)
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            mipmap: true
            sourceSize.height: 512
        }
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeWidth: well.activeFocus && FocusInput.keyboard || dropArea.containsDrag ? 2 : 1
                strokeColor: well.activeFocus && FocusInput.keyboard || dropArea.containsDrag ? root.look.accent : root.look.hairlineStrong
                strokeStyle: ShapePath.DashLine
                dashPattern: [4, 3]
                fillColor: "transparent"
                PathRectangle {
                    x: 0.5; y: 0.5
                    width: well.width - 1
                    height: well.height - 1
                    radius: root.look.params.cornerWell * root.look.unit
                }
            }
        }
        Text {
            anchors { centerIn: parent }
            width: parent.width - 4 * root.look.unit
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            visible: root.current === "" || shown.status === Image.Error
            text: qsTr("Drop, paste or browse")
            color: root.look.label
            font.pixelSize: root.look.textSize
        }
        DropArea {
            id: dropArea
            anchors.fill: parent
            onDropped: event => {
                if (event.hasUrls)
                    root.notice = root.appView.setImageFiles(root.field.key, event.urls)
            }
        }
        HoverHandler { id: wellHover }
        // A click only focuses the well (Ctrl+V pastes there); the Browse button is the one way to the file picker.
        TapHandler { onTapped: well.forceActiveFocus() }
        // Takes the picture away; the well is empty, and Run says what it needs.
        KitChip {
            objectName: "appImageRemove-" + root.field.key
            anchors { top: parent.top; right: parent.right; margins: root.look.unit }
            visible: root.current !== "" && (wellHover.hovered || activeFocus)
            look: root.look
            round: true
            frosted: true
            backdrop: shown
            backdropSource: shown.source
            glyph: "x"
            tip: qsTr("Remove the picture")
            onTapped: root.remove()
        }
    }
    Text {
        objectName: "appImageNotice-" + root.field.key
        Layout.fillWidth: true
        visible: root.notice !== ""
        text: root.notice
        color: root.look.error
        font.pixelSize: root.look.labelSize
        wrapMode: Text.Wrap
    }
    FilePicker {
        id: picker
        objectName: "appImagePicker-" + root.field.key
        title: root.field.label
        fileMode: PortalFileDialog.OpenFile
        nameFilters: [qsTr("Pictures (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"), qsTr("All files (*)")]
        onAccepted: root.notice = root.appView.setImageFiles(root.field.key, [selectedFile])
    }
}
