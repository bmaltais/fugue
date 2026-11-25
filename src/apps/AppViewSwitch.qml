import QtQuick
import Fugue

// The App | Graph switch of the window bar (#397): for a tab whose workflow has an app block, two segments, the one in force
// marked. Pressing the other segment runs view.toggle-app, as the key and the Window menu do; the segment in force does
// nothing. Made by MenuBar.qml only while the shown workflow is an app.
Rectangle {
    id: sw
    objectName: "appViewSwitch"
    required property CanvasLook look
    // True while the App view is shown (AppView.shown).
    property bool appShown: false

    implicitWidth: appSegment.implicitWidth + graphSegment.implicitWidth
    implicitHeight: 6 * look.unit
    radius: look.smallCorner
    color: look.well
    border { width: 1; color: look.hairlineStrong }
    Accessible.role: Accessible.RadioButton
    Accessible.name: qsTr("View")

    component Segment: Item {
        id: segment
        required property CanvasLook look
        required property string label
        required property bool current
        signal chosen
        implicitWidth: text.implicitWidth + 5 * segment.look.unit
        height: parent.height
        activeFocusOnTab: true
        Accessible.role: Accessible.RadioButton
        Accessible.name: label
        Accessible.checked: current
        Accessible.onPressAction: chosen()
        Keys.onSpacePressed: chosen()
        Keys.onReturnPressed: chosen()
        Rectangle {
            anchors { fill: parent; margins: 1 }
            radius: segment.look.smallCorner
            color: segment.current ? segment.look.body : hover.hovered ? Qt.tint(segment.look.well, Qt.alpha(segment.look.text, 0.05)) : "transparent"
            border { width: segment.activeFocus ? 2 : 0; color: segment.look.accent }
        }
        Text {
            id: text
            anchors.centerIn: parent
            text: segment.label
            font.pixelSize: segment.look.labelSize
            font.weight: segment.current ? Font.DemiBold : Font.Normal
            color: segment.current ? segment.look.text : segment.look.label
        }
        HoverHandler { id: hover }
        TapHandler { onTapped: { segment.forceActiveFocus(); segment.chosen() } }
    }

    Row {
        anchors.fill: parent
        Segment {
            id: appSegment
            objectName: "appViewSwitchApp"
            look: sw.look
            label: qsTr("App")
            current: sw.appShown
            onChosen: if (!sw.appShown) App.commands.trigger("view.toggle-app")
        }
        Segment {
            id: graphSegment
            objectName: "appViewSwitchGraph"
            look: sw.look
            label: qsTr("Graph")
            current: !sw.appShown
            onChosen: if (sw.appShown) App.commands.trigger("view.toggle-app")
        }
    }
}
