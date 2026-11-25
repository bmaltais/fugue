pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Fugue
import "../canvas"

// A row that opens (docs/canvas-look.md 20.6a): one line, `[glyph] Name ...... Where ...... Status [trailing] [chevron]`, 10u tall, and
// under it, while `expanded`, a body. A row of a KitGroup; a list of them is one group. The header is the button: a click, Enter
// or Space asks to open or close (`expandRequested`), Right opens and Left closes (swapped in a mirrored layout), Up and Down step
// to the next row (`stepRequested(+-1)`), Home and End to the first and last (`+-1000`), Alt+Up and Alt+Down ask to move the row
// (`moveRequested(+-1)`). Escape closes an open row from the header or from anywhere in its body, the focus staying on the header;
// a closed row leaves Escape to the window. Controlled, like the kit's other controls: the row never sets `expanded` itself.
//
// `detail` is the dim "where" column, `detailWidth` its shared width in px (0 hides it); it hides on its own under 110u of row
// width, and under 80u the status moves under the name and the row grows to 13u. `trailing` is the end controls (a KitToggle); a click
// on them is theirs. `tray` puts the body in a recessed tray inset 2u from each side and 2u above the next row; `tray: false` leaves the
// body to the caller (a list of rows). Height and chevron move over `durToggle`, the body fades over `durColor` (both 0 under
// reduced motion). `tabStop` makes the header a Tab stop: a list gives it to one row (the last one used), so Tab does not walk every row.
Item {
    id: row
    property CanvasLook look: App.theme.look // App.theme.look unless a host gives another
    property string label
    property string glyph
    property string detail
    property real detailWidth: 0
    property string status
    property bool statusStrong: false
    property bool expanded: false
    property bool tray: true
    property bool tabStop: true
    property string rowKey // what the host calls this row (a service's name): it finds the row again after it rebuilds its list
    property string statusName // objectName of the status text, for tests
    property string bodyName // objectName of the body
    property alias bodyEnabled: bodyGroup.enabled // false: the body's controls are dimmed and do nothing (the header still opens it)
    property bool animate: true // false: the next change of height jumps (the page closes a row it scrolls past at once)
    default property alias body: bodyGroup.rows
    property alias trailing: slot.data
    signal expandRequested(bool value)
    signal stepRequested(int delta)
    signal moveRequested(int delta)

    readonly property bool mirrored: LayoutMirroring.enabled
    readonly property bool narrow: width < 80 * look.unit
    readonly property bool showDetail: detailWidth > 0 && detail !== "" && width >= 110 * look.unit
    readonly property Item header: head
    // The height the body has when open (its rows, and the 2u under a tray), and the height it has now.
    readonly property real bodyHeight: bodyGroup.height + (tray ? 2 * look.unit : 0)
    property real shown: expanded ? bodyHeight : 0
    readonly property bool animating: heightAnimation.running
    property bool ready: false

    function focusHeader() { head.forceActiveFocus(Qt.OtherFocusReason) }

    implicitHeight: head.height + shown
    width: parent ? parent.width : implicitWidth
    height: implicitHeight
    Behavior on shown {
        enabled: row.ready && row.animate
        NumberAnimation { id: heightAnimation; duration: row.look.params.durToggle; easing.type: Easing.OutCubic }
    }
    Timer { running: true; interval: 150; onTriggered: row.ready = true } // a row made open starts open, not growing

    Keys.onEscapePressed: event => {
        if (row.expanded) {
            head.forceActiveFocus(Qt.OtherFocusReason)
            row.expandRequested(false)
            event.accepted = true
        } else {
            event.accepted = false
        }
    }

    Item {
        id: head
        width: parent.width
        height: (row.narrow ? 13 : 10) * row.look.unit
        activeFocusOnTab: row.tabStop && row.enabled
        Accessible.role: Accessible.Button
        Accessible.name: row.label
        Accessible.description: [row.detail, row.status].filter(t => t !== "").join(", ")
        Accessible.expandable: true
        Accessible.expanded: row.expanded
        Accessible.onPressAction: row.expandRequested(!row.expanded)

        KitSeparator {
            look: row.look
            visible: row.y > 0 && row.look.params.rowSeparators === "hairline"
        }
        Rectangle { // the hover fill, inset so it clears the group's rounded corners
            anchors { fill: parent; margins: row.look.unit }
            radius: Math.min(row.look.params.cornerWell * row.look.unit, height / 2)
            color: hover.hovered ? row.look.menuRow : "transparent"
            border.width: head.activeFocus && FocusInput.keyboard ? 2 : 0
            border.color: row.look.accent
        }
        RowLayout {
            anchors {
                fill: parent
                leftMargin: row.look.params.rowPadLeft * row.look.unit
                rightMargin: row.look.params.rowPadRight * row.look.unit
            }
            spacing: row.look.params.rowGap * row.look.unit
            Glyph {
                look: row.look
                name: row.glyph
                visible: row.glyph !== ""
                color: row.look.label
                Layout.alignment: Qt.AlignVCenter
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: Math.min(nameText.implicitWidth, 16 * row.look.unit)
                Layout.alignment: Qt.AlignVCenter
                spacing: 0.5 * row.look.unit
                Text {
                    id: nameText
                    Layout.fillWidth: true
                    text: row.label
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: row.look.text
                    font.pixelSize: Math.round(row.look.textSize * 7 / 6)
                }
                Text { // a narrow row says its status here
                    Layout.fillWidth: true
                    visible: row.narrow && row.status !== ""
                    text: row.status
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: row.statusStrong ? row.look.text : row.look.label
                    font.pixelSize: row.look.labelSize
                }
            }
            Text {
                Layout.preferredWidth: row.detailWidth
                Layout.alignment: Qt.AlignVCenter
                visible: row.showDetail
                text: row.detail
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: row.look.label
                font.pixelSize: row.look.labelSize
            }
            Text {
                id: statusText
                objectName: row.statusName
                Layout.maximumWidth: 26 * row.look.unit
                Layout.alignment: Qt.AlignVCenter
                visible: !row.narrow && row.status !== ""
                text: row.status
                textFormat: Text.PlainText
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignRight
                color: row.statusStrong ? row.look.text : row.look.label
                font.pixelSize: row.look.labelSize
                HoverHandler { id: statusHover }
                ToolTip.visible: statusHover.hovered && statusText.truncated
                ToolTip.delay: row.look.tooltipDelay
                ToolTip.text: row.status
            }
            Row {
                id: slot
                Layout.alignment: Qt.AlignVCenter
                spacing: 2 * row.look.unit
            }
            Glyph { // a decoration: the header is the button
                look: row.look
                name: row.mirrored ? "chevron-left" : "chevron-right"
                color: row.look.label
                Layout.alignment: Qt.AlignVCenter
                rotation: row.expanded ? (row.mirrored ? -90 : 90) : 0
                Behavior on rotation { enabled: row.ready; NumberAnimation { duration: row.look.params.durToggle; easing.type: Easing.OutCubic } }
            }
        }
        HoverHandler { id: hover }
        TapHandler {
            gesturePolicy: TapHandler.WithinBounds
            onTapped: point => {
                const at = slot.mapFromItem(head, point.position)
                if (slot.width > 0 && at.x >= 0 && at.x <= slot.width && at.y >= 0 && at.y <= slot.height)
                    return // the trailing controls take their own taps
                head.forceActiveFocus(Qt.MouseFocusReason)
                row.expandRequested(!row.expanded)
            }
        }
        Keys.onPressed: event => {
            if (!head.activeFocus)
                return
            const alt = (event.modifiers & Qt.AltModifier) !== 0
            event.accepted = true
            switch (event.key) {
            case Qt.Key_Up: alt ? row.moveRequested(-1) : row.stepRequested(-1); break
            case Qt.Key_Down: alt ? row.moveRequested(1) : row.stepRequested(1); break
            case Qt.Key_Home: row.stepRequested(-1000); break
            case Qt.Key_End: row.stepRequested(1000); break
            case Qt.Key_Return:
            case Qt.Key_Enter:
            case Qt.Key_Space: row.expandRequested(!row.expanded); break
            case Qt.Key_Right: row.expandRequested(!row.mirrored); break
            case Qt.Key_Left: row.expandRequested(row.mirrored); break
            default: event.accepted = false
            }
        }
    }

    Item { // the body: its height is what shows, the tray inside is its full size
        id: bodyHost
        objectName: row.bodyName
        y: head.height
        width: parent.width
        height: row.shown
        clip: true
        visible: row.expanded || row.shown > 0.5
        opacity: row.expanded ? 1 : 0
        Behavior on opacity { enabled: row.ready; NumberAnimation { duration: row.look.params.durColor; easing.type: Easing.OutCubic } }
        Rectangle { // the recessed tray
            x: row.tray ? 2 * row.look.unit : 0
            width: parent.width - 2 * x
            height: bodyGroup.height
            radius: row.tray ? Math.min(row.look.params.cornerWell * row.look.unit, 0.3 * height) : 0
            color: row.tray ? Qt.rgba(0, 0, 0, row.look.light ? 0.04 : 0.16) : "transparent"
            KitGroup {
                id: bodyGroup
                look: row.look
                bare: true
                width: parent.width
            }
        }
    }
}
