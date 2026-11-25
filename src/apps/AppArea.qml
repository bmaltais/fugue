pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../workspace"

// The App view in an area (#450, spec 12.16 and 12.17): AppPage.qml over the one AppView store (App.appView), which keeps its form,
// runs and takes per tab. The page is made only while the shown workflow is an app, so an area that shows the App over a workflow
// with no app block costs one line of text; the window's one area shows the App view as a tab mode of Main.qml, as before.
AreaView {
    id: root
    objectName: "appArea"
    readonly property bool ready: App.appView !== null && App.appView.isApp
    readonly property CanvasLook look: App.theme.look

    Loader {
        id: page
        anchors.fill: parent
        active: root.ready
        focus: true
        sourceComponent: AppPage {}
    }
    Text {
        objectName: "appAreaNote"
        visible: !root.ready
        anchors.centerIn: parent
        width: parent.width - 8 * root.look.unit
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        color: root.look.label
        font.pixelSize: root.look.labelSize
        text: qsTr("This workflow is not an app.")
    }
}
