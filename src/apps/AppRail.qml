pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../kit"

// The App view's slim rail at the start edge (the layout of ComfyUI App Mode's icon rail): Apps (the shelf of apps, to pick
// another: the first item and the way back to the list), Results (this app's runs and the pictures to try) and, in the full
// window, Graph (the workflow behind the app). Each is an icon over a short word (ComfyUI's rail is icons only and the owner did
// not find the way back to the list), reached with Tab, worked with Enter or Space, a tooltip with the key where there is one,
// and mirrored at the end edge in right-to-left like any row. `standalone` is the app's own window, which has no graph to open.
Rectangle {
    id: rail
    objectName: "appRail"
    required property CanvasLook look
    property bool standalone: false
    signal appsRequested
    signal resultsRequested
    signal graphRequested

    implicitWidth: 14 * look.unit
    // A rounded card like the stage and the panel, with their hairline edge.
    radius: look.popupCorner
    color: look.popup
    border.width: 1
    border.color: look.hairline
    component RailItem: Item {
        id: item
        required property string glyph
        required property string label
        property string tip
        signal tapped
        Layout.fillWidth: true
        Layout.leftMargin: rail.look.unit
        Layout.rightMargin: rail.look.unit
        implicitHeight: 11.5 * rail.look.unit
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: label
        Accessible.onPressAction: item.tapped()
        Keys.onReturnPressed: item.tapped()
        Keys.onEnterPressed: item.tapped()
        Keys.onSpacePressed: item.tapped()
        Rectangle {
            anchors.fill: parent
            radius: rail.look.smallCorner + 2
            color: tap.pressed ? rail.look.menuRow : hover.hovered ? rail.look.kitChipHover : rail.look.kitChip
            border.width: item.activeFocus ? 2 : 0
            border.color: rail.look.accent
            KitRaise { look: rail.look; radius: parent.radius }
        }
        Column {
            anchors.centerIn: parent
            spacing: rail.look.unit / 2
            Glyph {
                anchors.horizontalCenter: parent.horizontalCenter
                look: rail.look
                name: item.glyph
                color: item.activeFocus || hover.hovered ? rail.look.text : rail.look.label
                font.pixelSize: 18
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: item.label
                textFormat: Text.PlainText
                color: item.activeFocus || hover.hovered ? rail.look.text : rail.look.label
                font.pixelSize: rail.look.labelSize
            }
        }
        TapHandler { id: tap; gesturePolicy: TapHandler.WithinBounds; onTapped: item.tapped() }
        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
        ToolTip.visible: hover.hovered && item.tip !== ""
        ToolTip.delay: rail.look.tooltipDelay
        ToolTip.text: item.tip
    }
    ColumnLayout {
        anchors { fill: parent; topMargin: 2 * rail.look.unit; bottomMargin: 2 * rail.look.unit }
        spacing: rail.look.unit
        RailItem {
            objectName: "appRailApps"
            glyph: "layout-template"
            label: qsTr("Apps")
            tip: qsTr("All apps (%1)").arg((App.commands.entry("app.show-all").keys ?? [])[0] ?? "")
            onTapped: rail.appsRequested()
        }
        RailItem {
            objectName: "appRailResults"
            glyph: "history"
            label: qsTr("Results")
            tip: qsTr("Results and pictures")
            onTapped: rail.resultsRequested()
        }
        Item { Layout.fillHeight: true }
        RailItem {
            objectName: "appOpenInGraph"
            visible: !rail.standalone
            glyph: "workflow"
            label: qsTr("Graph")
            tip: qsTr("Open in Graph: the workflow behind this app")
            onTapped: rail.graphRequested()
        }
    }
}
