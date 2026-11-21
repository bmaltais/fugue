pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue

// The frame of a large image view (#62, #73): a modal popup at 90% of the window with a bar (the
// caller's `tools`, then Fit and 100%, then the close button), the viewer filling the rest and an
// optional `footer` under it. It knows nothing of nodes: give it the `zoomable` (the viewer: anything with `fit()`,
// `actualSize()`, `zoomAt()`, `actual` and `zoom`, as CompareView has) and the buttons it wants in
// the bar; a viewer that does not zoom (a player) leaves `zoomable` empty, and the bar then has no
// Fit and 100%. Keys: 0 Fit, 1 100%, + and - zoom about the middle, Escape closes (Popup's policy),
// and `confirm` is Enter.
//
// `name` prefixes the objectNames of the bar and its buttons ("compare": compareBar, compareZoom-fit,
// compareZoom-actual, compareClose), so a caller's tests find them.
Popup {
    id: root
    required property CanvasLook look
    required property string name
    property var zoomable: null
    property alias tools: toolsRow.data
    default property alias body: bodyHolder.data
    property alias footer: footerHolder.data
    readonly property alias bar: bar
    signal confirm // Enter, when nothing in the dialog took it (a text field does)

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.round(parent.width * 0.9)
    height: Math.round(parent.height * 0.9)
    padding: 2 * look.unit
    modal: true
    focus: true
    enter: null
    exit: null
    closePolicy: Popup.CloseOnEscape
    background: PopupSurface { look: root.look }

    contentItem: Column {
        id: column
        spacing: 2 * root.look.unit
        focus: true
        Keys.onPressed: event => {
            if (!root.zoomable || (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)))
                return
            const middle = Qt.point(root.zoomable.width / 2, root.zoomable.height / 2)
            if (event.key === Qt.Key_0) root.zoomable.fit()
            else if (event.key === Qt.Key_1) root.zoomable.actualSize()
            else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) root.zoomable.zoomAt(middle.x, middle.y, 1.25)
            else if (event.key === Qt.Key_Minus) root.zoomable.zoomAt(middle.x, middle.y, 0.8)
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) root.confirm()
            else return
            event.accepted = true
        }
        Row {
            id: bar
            objectName: root.name + "Bar"
            spacing: root.look.unit
            readonly property real h: 7 * root.look.unit // 28px: above the 24px floor
            Row {
                id: toolsRow
                spacing: root.look.unit
            }
            Item { width: 2 * root.look.unit; height: 1; visible: !!root.zoomable } // a gap of its own between the chunks
            ViewButton {
                objectName: root.name + "Zoom-fit"
                visible: !!root.zoomable
                look: root.look
                text: qsTr("Fit")
                accented: false
                current: root.zoomable && !root.zoomable.actual && root.zoomable.zoom === 1
                onTapped: root.zoomable.fit()
            }
            ViewButton {
                objectName: root.name + "Zoom-actual"
                visible: !!root.zoomable
                look: root.look
                text: qsTr("100%")
                accented: false
                current: root.zoomable && root.zoomable.actual
                onTapped: root.zoomable.actualSize()
            }
            Item { width: 2 * root.look.unit; height: 1 }
            ViewButton {
                objectName: root.name + "Close"
                look: root.look
                text: "×"
                onTapped: root.close()
            }
        }
        Item {
            id: bodyHolder
            width: parent.width
            height: parent.height - bar.height - parent.spacing - (footerHolder.visible ? footerHolder.height + parent.spacing : 0)
        }
        Item {
            id: footerHolder
            width: parent.width
            visible: childrenRect.height > 0
            height: childrenRect.height
        }
    }
}
