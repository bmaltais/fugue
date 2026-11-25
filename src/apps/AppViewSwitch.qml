import QtQuick
import Fugue
import "../kit"

// The App | Graph switch of the window bar (#397): for a tab whose workflow has an app block, a KitSegmented of two options, the one
// in force chosen. Choosing the other runs view.toggle-app, as the key and the Window menu do; the one in force does nothing. It is
// in the Dusk tone, the App view's. Made by MenuBar.qml only while the shown workflow is an app.
Item {
    id: sw
    objectName: "appViewSwitch"
    required property CanvasLook look
    // True while the App view is shown (AppView.shown).
    property bool appShown: false

    implicitWidth: seg.implicitWidth
    implicitHeight: seg.implicitHeight
    Accessible.role: Accessible.RadioButton
    Accessible.name: qsTr("View")

    KitSegmented {
        id: seg
        look: sw.look
        tone: "dusk"
        options: [qsTr("App"), qsTr("Graph")]
        optionNames: ["appViewSwitchApp", "appViewSwitchGraph"]
        currentIndex: sw.appShown ? 0 : 1
        text: qsTr("View")
        onMoved: index => {
            if ((index === 0) !== sw.appShown)
                App.commands.trigger("view.toggle-app")
        }
    }
}
