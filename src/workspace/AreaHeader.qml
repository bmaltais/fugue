pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "../canvas/widgets"

// The bar at the top of an area while the window has more than one (#217 slice 1, spec 12.16; docs/ui-ergonomics.md):
// the view-type menu at the start (the view's glyph, its name and a chevron; Blender's editor-type menu), then, at
// the end, split side by side, split top and bottom and, a chunk of its own, close. Between them, the view's own menus
// (#262, as Blender's editor headers: the Graph's View, Select, Add and Node; folded into one Menu button where they
// do not fit). The window's global menus are the window bar's. The active area's bar has an
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

    // A square 6u (24px) button of the bar with a Lucide glyph (canvas-look 16): columns-2 and rows-2 for the splits.
    // The window's one button (ViewButton, D4) in its flat and compact variants; a thin alias.
    component BarButton: ViewButton {
        look: header.look
        flat: true
        compact: true
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

    // The view's own menus, after the view-type menu, in the room between it and the splits. The Graph's here; the Canvas
    // view keeps its own bar inside the view (the File > Canvas… dialog has no header).
    Loader {
        id: menus
        objectName: "areaMenus-" + header.areaId
        active: header.viewType === "graph"
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: typeButton.right
        anchors.leftMargin: header.u
        sourceComponent: CanvasMenuBar {
            look: header.look
            treeName: "graph"
            idPrefix: "graph"
            maxWidth: Math.max(0, splits.x - typeButton.x - typeButton.width - 3 * header.u)
        }
    }

    // Split, split, then close in a chunk of its own (2u from the splits against their 0).
    Row {
        id: splits
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: parent.right
        anchors.rightMargin: header.endClear
        spacing: 0
        BarButton {
            objectName: "areaSplitRow-" + header.areaId
            glyph: "columns-2"
            tip: header.tipOf(qsTr("Split side by side"), "area.split-row")
            // Read again when the layout or the window's size changes.
            enabled: App.workspace !== null && App.workspace.count > 0 && App.workspace.bodySize.width >= 0
                     && App.workspace.canSplit(header.areaId, "row")
            onTapped: header.run("area.split-row")
        }
        BarButton {
            objectName: "areaSplitColumn-" + header.areaId
            glyph: "rows-2"
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
            enabled: App.workspace !== null && App.workspace.count > 1
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
