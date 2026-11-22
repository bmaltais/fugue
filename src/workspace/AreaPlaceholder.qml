import QtQuick
import QtQuick.Controls
import Fugue

// What an area shows for a view type this build does not register (a newer Fugue's, kept in a saved layout:
// workspace/area_tree.h). The type stays in the tree and is written back unchanged; from slice 1 the area's
// header picks another view.
Rectangle {
    id: placeholder
    objectName: "areaPlaceholder"
    property string viewType
    color: App.theme.look.canvas
    border.color: App.theme.look.hairline
    Accessible.role: Accessible.StaticText
    Accessible.name: message.text
    Label {
        id: message
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 4 * App.theme.look.unit }
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        color: App.theme.look.label
        text: qsTr("This area shows a view this version of Fugue does not have (%1).").arg(placeholder.viewType)
    }
}
