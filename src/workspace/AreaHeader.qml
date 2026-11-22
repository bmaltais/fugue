pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "../canvas/widgets"

// The bar at the top of an area while the window has more than one (#217 slice 1, spec 12.16; docs/ui-ergonomics.md):
// the view-type menu at the start (the view's glyph, its name and a chevron; Blender's editor-type menu), then, at
// the end, split side by side, split top and bottom and, a chunk of its own, close. The active area's bar has an
// accent line along its foot and its name in the text color; the others are quieter. AreaNode.qml makes one per
// area (none while the window has one area: the default window has no header). It reads App.workspace and runs the
// area commands of the area it heads, which it makes the active one first.
Rectangle {
    id: header
    objectName: "areaHeader-" + areaId
    property int areaId: 0
    property string viewType: ""
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property bool active: App.workspace !== null && App.workspace.active === areaId
    // The ends stay clear of the border handles that reach into the bar (AreaHandle.qml: half its thickness).
    readonly property real endClear: Math.max(12, 3 * u)
    // How far the window's furniture reaches into this bar's start (the auto-hide menu bar's handle, AreaNode.qml).
    property real leadingClear: 0
    color: look.popup
    Accessible.role: Accessible.ToolBar
    Accessible.name: qsTr("%1 area").arg(App.workspace ? App.workspace.views.name(viewType) : viewType)

    function keyOf(command) {
        const keys = App.commands.entry(command).keys ?? []
        return keys.length > 0 ? keys[0] : ""
    }
    function tipOf(text, command) {
        const key = keyOf(command)
        return key !== "" ? qsTr("%1 (%2)").arg(text).arg(key) : text
    }
    function run(command) {
        App.workspace.active = areaId
        App.commands.trigger(command)
    }
    function openViews() {
        App.workspace.active = areaId
        const entries = App.workspace.viewChoices(areaId).map(choice => ({
            text: choice.name, value: choice.id, checked: choice.current, enabled: choice.enabled || choice.current,
            detail: choice.moves ? qsTr("moves here") : !choice.enabled ? qsTr("the Graph keeps an area") : ""}))
        const at = typeButton.mapToItem(Overlay.overlay, 0, typeButton.height)
        const menu = menuComponent.createObject(header, {wantX: at.x, wantY: at.y, width: Math.max(typeButton.width, 40 * u),
                                                         anchorHeight: typeButton.height, entries: entries}) as WidgetMenu
        menu.picked.connect(value => App.workspace.showView(header.areaId, value))
        menu.open()
    }

    Component {
        id: menuComponent
        WidgetMenu {
            id: menu
            objectName: "areaViewMenu"
            look: header.look
            parent: Overlay.overlay
            onClosed: menu.destroy()
        }
    }

    // A square 6u (24px) button of the bar: a glyph, or a small drawn picture of the split it makes.
    component BarButton: Rectangle {
        id: button
        property string glyph
        property string split: "" // "row" or "column": draws the split instead of a glyph
        property string tip
        property bool danger: false
        signal tapped
        width: 6 * header.u
        height: 6 * header.u
        radius: header.look.smallCorner
        color: !enabled ? "transparent"
             : danger && (hover.hovered || tap.pressed) ? header.look.dangerHover
             : tap.pressed ? header.look.selectedRow : hover.hovered ? header.look.menuRow : "transparent"
        border.width: activeFocus ? 2 : 0
        border.color: danger ? header.look.error : header.look.accent
        opacity: enabled ? 1 : 0.4
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: tip
        readonly property color ink: hover.hovered || activeFocus ? header.look.text : header.look.label
        Glyph {
            visible: button.split === ""
            anchors.centerIn: parent
            look: header.look
            name: button.glyph
            color: button.danger && hover.hovered ? header.look.error : button.ink
        }
        // The split as a box with its new border.
        Rectangle {
            visible: button.split !== ""
            anchors.centerIn: parent
            width: 14
            height: 12
            radius: 2
            color: "transparent"
            border.width: 1.5
            border.color: button.ink
            Rectangle {
                anchors.centerIn: parent
                width: button.split === "row" ? 1.5 : parent.width
                height: button.split === "row" ? parent.height : 1.5
                color: button.ink
            }
        }
        TapHandler {
            id: tap
            onTapped: button.tapped()
        }
        HoverHandler {
            id: hover
            cursorShape: button.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
        ToolTip.visible: hover.hovered && button.tip !== ""
        ToolTip.delay: header.look.tooltipDelay
        ToolTip.text: button.tip
        Keys.onReturnPressed: button.tapped()
        Keys.onSpacePressed: button.tapped()
    }

    // The view-type menu.
    Rectangle {
        id: typeButton
        objectName: "areaType-" + header.areaId
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.leftMargin: Math.max(header.endClear, header.leadingClear + header.u)
        width: typeRow.implicitWidth + 3 * header.u
        height: 6 * header.u
        radius: header.look.smallCorner
        color: typeTap.pressed ? header.look.selectedRow : typeHover.hovered ? header.look.menuRow : "transparent"
        border.width: activeFocus ? 2 : 0
        border.color: header.look.accent
        activeFocusOnTab: true
        Accessible.role: Accessible.ButtonMenu
        Accessible.name: qsTr("View: %1").arg(typeLabel.text)
        Row {
            id: typeRow
            anchors.centerIn: parent
            spacing: header.u
            Glyph {
                anchors.verticalCenter: parent.verticalCenter
                look: header.look
                name: App.workspace ? App.workspace.views.glyph(header.viewType) : ""
                color: header.active ? header.look.accent : header.look.label
            }
            Text {
                id: typeLabel
                anchors.verticalCenter: parent.verticalCenter
                text: App.workspace ? App.workspace.views.name(header.viewType) : header.viewType
                color: header.active ? header.look.text : header.look.label
                font.pixelSize: header.look.labelSize
            }
            Glyph {
                anchors.verticalCenter: parent.verticalCenter
                look: header.look
                name: "chevron-down"
                color: header.look.label
            }
        }
        TapHandler {
            id: typeTap
            onTapped: header.openViews()
        }
        HoverHandler {
            id: typeHover
            cursorShape: Qt.PointingHandCursor
        }
        ToolTip.visible: typeHover.hovered
        ToolTip.delay: header.look.tooltipDelay
        ToolTip.text: qsTr("Choose what this area shows")
        Keys.onReturnPressed: header.openViews()
        Keys.onSpacePressed: header.openViews()
    }

    // Split, split, then close in a chunk of its own (2u from the splits against their 0).
    Row {
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: parent.right
        anchors.rightMargin: header.endClear
        spacing: 0
        BarButton {
            objectName: "areaSplitRow-" + header.areaId
            split: "row"
            tip: header.tipOf(qsTr("Split side by side"), "area.split-row")
            // Read again when the layout or the window's size changes.
            enabled: App.workspace !== null && App.workspace.count > 0 && App.workspace.bodySize.width >= 0
                     && App.workspace.canSplit(header.areaId, "row")
            onTapped: header.run("area.split-row")
        }
        BarButton {
            objectName: "areaSplitColumn-" + header.areaId
            split: "column"
            tip: header.tipOf(qsTr("Split top and bottom"), "area.split-column")
            enabled: App.workspace !== null && App.workspace.count > 0 && App.workspace.bodySize.height >= 0
                     && App.workspace.canSplit(header.areaId, "column")
            onTapped: header.run("area.split-column")
        }
        Item { width: 2 * header.u; height: 1 }
        BarButton {
            objectName: "areaClose-" + header.areaId
            glyph: "x"
            danger: true
            tip: header.tipOf(qsTr("Close area"), "area.close")
            onTapped: header.run("area.close")
        }
    }

    // The foot: a hairline, the accent on the active area.
    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: header.active ? 2 : 1
        color: header.active ? header.look.accent : header.look.hairline
    }
}
