pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../menu"

// A picture input: a well that shows the picture and takes one dropped on it (Enter or a click opens the file picker, Ctrl+V
// pastes), Browse and Paste buttons, and a strip of pictures to try: the template's samples first, then the last used
// (the inputs folder's newest). AppView takes the file into the inputs folder (setImageFiles, pasteImage, useImage); this
// shows `valueOf(key)`. A refusal is one line under the strip, in the error color.
ColumnLayout {
    id: root
    required property var field
    required property AppView appView
    required property CanvasLook look
    property string current: field.value === undefined || field.value === null ? "" : String(field.value)
    property string notice
    property var recents: appView.recentImages()
    // Samples, then the recent ones the samples do not already show, at most ten.
    readonly property var strip: {
        const paths = field.samples.map(s => s.path)
        return field.samples.concat(recents.filter(r => !paths.includes(r.path))).slice(0, 10)
    }
    spacing: look.unit

    Connections {
        target: root.appView
        function onValueChanged(key) {
            if (key !== root.field.key)
                return
            root.current = String(root.appView.valueOf(key) ?? "")
            root.recents = root.appView.recentImages()
        }
    }
    function paste() { root.notice = root.appView.pasteImage(root.field.key) }

    Rectangle {
        id: well
        objectName: "appInput-" + root.field.key
        Layout.fillWidth: true
        implicitHeight: 36 * root.look.unit
        radius: root.look.smallCorner
        color: root.look.well
        border.width: activeFocus || drop.containsDrag ? 2 : 1
        border.color: activeFocus || drop.containsDrag ? root.look.accent : root.look.hairline
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: root.field.label
        Accessible.description: root.current === "" ? qsTr("No picture") : root.current.split("/").pop()
        Keys.onReturnPressed: picker.open()
        Keys.onEnterPressed: picker.open()
        Keys.onSpacePressed: picker.open()
        Keys.onPressed: event => {
            if (event.matches(StandardKey.Paste)) {
                root.paste()
                event.accepted = true
            }
        }
        Image {
            anchors { fill: parent; margins: 2 }
            source: root.appView.fileUrl(root.current)
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            mipmap: true
            sourceSize.height: 256
        }
        Text {
            anchors.centerIn: parent
            visible: root.current === ""
            text: qsTr("Drop a picture here, paste one or browse")
            color: root.look.label
            font.pixelSize: root.look.textSize
        }
        DropArea {
            id: drop
            anchors.fill: parent
            onDropped: event => {
                if (event.hasUrls)
                    root.notice = root.appView.setImageFiles(root.field.key, event.urls)
            }
        }
        TapHandler { onTapped: { well.forceActiveFocus(); picker.open() } }
    }
    RowLayout {
        spacing: root.look.unit
        ViewButton {
            objectName: "appImageBrowse-" + root.field.key
            look: root.look
            text: qsTr("Browse…")
            glyph: "folder"
            bordered: true
            onTapped: picker.open()
        }
        ViewButton {
            objectName: "appImagePaste-" + root.field.key
            look: root.look
            text: qsTr("Paste")
            bordered: true
            onTapped: root.paste()
        }
    }
    ListView {
        id: thumbs
        objectName: "appImageStrip-" + root.field.key
        Layout.fillWidth: true
        implicitHeight: 14 * root.look.unit
        visible: root.strip.length > 0
        orientation: ListView.Horizontal
        spacing: root.look.unit
        clip: true
        model: root.strip
        activeFocusOnTab: true
        keyNavigationEnabled: true
        highlightFollowsCurrentItem: true
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Pictures to try")
        Keys.onReturnPressed: pick(currentIndex)
        Keys.onEnterPressed: pick(currentIndex)
        Keys.onSpacePressed: pick(currentIndex)
        function pick(index) {
            if (index >= 0 && index < root.strip.length)
                root.appView.useImage(root.field.key, root.strip[index].path)
        }
        delegate: Rectangle {
            id: thumb
            required property var modelData
            required property int index
            readonly property bool chosen: root.current !== "" && root.current === modelData.path
            width: 14 * root.look.unit
            height: 14 * root.look.unit
            radius: root.look.smallCorner
            color: root.look.well
            border.width: chosen || (thumbs.activeFocus && ListView.isCurrentItem) ? 2 : 1
            border.color: chosen || (thumbs.activeFocus && ListView.isCurrentItem) ? root.look.accent : root.look.hairline
            Accessible.role: Accessible.Button
            Accessible.name: modelData.name
            Image {
                anchors { fill: parent; margins: 2 }
                source: thumb.modelData.url
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                sourceSize.height: 96
            }
            TapHandler { onTapped: { thumbs.currentIndex = thumb.index; thumbs.pick(thumb.index) } }
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
