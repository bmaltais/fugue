pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../kit"

// A mask input: the mask editor (spec 14.4) over the form's picture. A button opens it; "Painted" or "None" says what is
// there, Clear takes it away. The picture is AppView's maskPicture() (the first picture input's now): with none chosen a
// line says so. The editor stores the mask in the inputs folder and says `saved(path)` (empty: nothing painted).
RowLayout {
    id: row
    required property var field
    required property AppView appView
    required property CanvasLook look
    property string current: field.value === undefined || field.value === null ? "" : String(field.value)
    property string notice
    property bool editorMade: false
    spacing: look.unit

    function setMask(path) {
        row.current = path
        row.appView.setValue(row.field.key, path)
    }
    function paint() {
        const picture = row.appView.maskPicture()
        if (picture.url === undefined) {
            row.notice = qsTr("Pick a picture first: there is nothing to paint on yet.")
            return
        }
        row.notice = ""
        row.editorMade = true
        const editor = loader.item as MaskEditor
        editor.imageSource = picture.url
        editor.imageSize = Qt.size(picture.width, picture.height)
        editor.maskPath = row.appView.resolvedPath(row.current)
        editor.open()
    }

    KitChip {
        objectName: "appInput-" + row.field.key
        look: row.look
        pill: true
        text: qsTr("Paint mask…")
        glyph: "paintbrush"
        onTapped: row.paint()
    }
    Text {
        objectName: "appMaskState-" + row.field.key
        Layout.fillWidth: true
        text: row.notice !== "" ? row.notice : row.current !== "" ? qsTr("Painted") : qsTr("None")
        color: row.notice !== "" ? row.look.error : row.look.label
        font.pixelSize: row.look.labelSize
        wrapMode: Text.Wrap
    }
    KitChip {
        objectName: "appMaskClear-" + row.field.key
        look: row.look
        pill: true
        text: qsTr("Clear")
        visible: row.current !== ""
        onTapped: row.setMask("")
    }
    // Made at the first Paint: the editor is a large view the form does not need until then.
    Loader {
        id: loader
        active: row.editorMade
        asynchronous: false
        sourceComponent: MaskEditor {
            look: App.theme.look
            onSaved: path => row.setMask(path)
        }
    }
}
