pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue

// The frame of a large image view (#62, #73): a modal popup at 90% of the window with a bar (the
// caller's `tools`, then Fit and 100%, then the close button), the viewer filling the rest and an
// optional `footer` under it. It knows nothing of nodes: give it the `zoomable` (the viewer: anything with `fit()`,
// `actualSize()`, `zoomAt()`, `actual` and `zoom`, as CompareView has) and the buttons it wants in
// the bar; a viewer that does not zoom (a player) leaves `zoomable` empty, and the bar then has no
// Fit and 100%. Keys (one keymap with the Graph, docs/ui-ergonomics.md D5): Home or 0 Fit, 1 100%, + and - zoom about
// the middle, Escape closes (Popup's policy), and `confirm` is Enter. `showBar: false` is for a body with a bar of its
// own (the Canvas view, which then holds Fit, 100% and the close at the end of its tools: one bar, as the Mask
// editor). Right to left the bar and the footer mirror, as Settings and Projects do; the body does not, so a picture
// is never mirrored (a body with controls of its own mirrors them itself).
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
    property bool showBar: true
    // The area the picture belongs to colours the card (docs/canvas-look.md 20.21): ember for the media and results, sea for the Canvas
    // and the paint window. A working window keeps its room, so the header is a soft fade of the tone behind the bar, not a hero.
    property string tone: "ember"
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    signal confirm // Enter, when nothing in the dialog took it (a text field does)
    // A key the dialog did not take, for a viewer with keys of its own (the mask editor's B and E):
    // the handler sets `event.accepted`. The release of any key too.
    signal keyPressed(var event)
    signal keyReleased(var event)

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
    background: Item {
        PopupSurface { anchors.fill: parent; look: root.look; tone: root.tone }
        Rectangle {
            objectName: "largeViewFade"
            x: 1; y: 1
            width: parent.width - 2
            height: root.topPadding + bar.h + root.look.unit
            visible: root.look.params.panelTint > 0.001 && root.look.params.areaLayout !== "panes"
            radius: Math.max(0, Math.min(root.look.params.cornerCard * root.look.unit, 0.2 * Math.min(parent.width, parent.height)) - 1)
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.alpha(root.look.tone(root.tone).blobs[0], 2.2 * root.look.params.panelTint) }
                GradientStop { position: 1; color: Qt.alpha(root.look.tone(root.tone).blobs[0], 0) }
            }
        }
    }
    onClosed: if (zoomable && zoomable.peekUp) zoomable.peekUp(true)
    onActiveFocusChanged: if (!activeFocus && zoomable && zoomable.peekUp) zoomable.peekUp(true)

    contentItem: Column {
        id: column
        spacing: 2 * root.look.unit
        focus: true
        Keys.onReleased: event => {
            // Z held peeks at 100% of the larger picture (a viewer with peekDown: Compare Images, #420).
            if (event.key === Qt.Key_Z && root.zoomable && root.zoomable.peekUp) {
                if (!event.isAutoRepeat)
                    root.zoomable.peekUp(false)
                event.accepted = true
                return
            }
            root.keyReleased(event)
        }
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Z && !(event.modifiers & ~Qt.ShiftModifier) && root.zoomable && root.zoomable.peekDown) {
                if (!event.isAutoRepeat)
                    root.zoomable.peekDown()
                event.accepted = true
                return
            }
            if (!root.zoomable || (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
                root.keyPressed(event)
                return
            }
            const middle = Qt.point(root.zoomable.width / 2, root.zoomable.height / 2)
            if (event.key === Qt.Key_Home || event.key === Qt.Key_0) root.zoomable.fit()
            else if (event.key === Qt.Key_1) root.zoomable.actualSize()
            else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) root.zoomable.zoomAt(middle.x, middle.y, 1.25)
            else if (event.key === Qt.Key_Minus) root.zoomable.zoomAt(middle.x, middle.y, 0.8)
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) root.confirm()
            else {
                root.keyPressed(event)
                return
            }
            event.accepted = true
        }
        Row {
            id: bar
            objectName: root.name + "Bar"
            visible: root.showBar
            spacing: root.look.unit
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
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
                tip: qsTr("Fit (Home)")
                accented: false
                current: root.zoomable && !root.zoomable.actual && root.zoomable.zoom === 1
                onTapped: root.zoomable.fit()
            }
            ViewButton {
                objectName: root.name + "Zoom-actual"
                visible: !!root.zoomable
                look: root.look
                text: qsTr("100%")
                tip: qsTr("Actual size (1)")
                accented: false
                current: root.zoomable && root.zoomable.actual
                onTapped: root.zoomable.actualSize()
            }
            Item { width: 2 * root.look.unit; height: 1 }
            ViewButton {
                objectName: root.name + "Close"
                look: root.look
                glyph: "x" // the Lucide x of every other close (canvas-look 16)
                tip: qsTr("Close (Esc)")
                onTapped: root.close()
            }
        }
        Item {
            id: bodyHolder
            width: parent.width
            height: parent.height - (bar.visible ? bar.height + parent.spacing : 0) - (footerHolder.visible ? footerHolder.height + parent.spacing : 0)
        }
        Item {
            id: footerHolder
            width: parent.width
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            // Shown when a footer was given. Not `childrenRect.height > 0`: a hidden holder hides its footer, whose rows (positioners and
            // layouts skip hidden items) then measure 0 and never show it again (the Region window's info lines, 2026-11-27).
            visible: children.length > 0
            height: childrenRect.height
        }
    }
}
