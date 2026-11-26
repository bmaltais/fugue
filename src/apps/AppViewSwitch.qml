import QtQuick
import QtQuick.Controls
import Fugue
import "../kit"

// The App | Graph switch of the window bar (#397): always shown, a KitSegmented of two options, the one in force chosen; pressing the
// chosen one does nothing, the other runs view.toggle-app as the key (Ctrl+Alt+A) and the Window menu do. What the other does depends on
// the tab: an app, its other view; the shelf, Start blank (App chosen); a plain graph, the app builder (Graph chosen; the segment reads
// "Make app…" there, so it never looks like a view switch that opens a dialog instead);
// the empty graph of Start blank, back to the shelf. It is in the Dusk tone, the App view's. Made by MenuBar.qml.
Item {
    id: sw
    objectName: "appViewSwitch"
    required property CanvasLook look
    // True while the App view is shown (AppView.shown).
    property bool appShown: false
    // The shelf is the body: App is chosen there too.
    property bool atHome: false
    // The tab is a plain graph with something in it: App opens the builder, and its segment says so ("Make app…").
    property bool plainGraph: false

    implicitWidth: seg.implicitWidth
    implicitHeight: seg.implicitHeight
    Accessible.role: Accessible.RadioButton
    Accessible.name: qsTr("View")

    KitSegmented {
        id: seg
        look: sw.look
        tone: "dusk"
        focusOnPress: false // a click leaves the keys with the page; Tab still reaches it
        options: [sw.plainGraph ? qsTr("Make app…") : qsTr("App"), qsTr("Graph")]
        optionNames: ["appViewSwitchApp", "appViewSwitchGraph"]
        currentIndex: sw.appShown || sw.atHome ? 0 : 1
        text: qsTr("View")
        onMoved: index => {
            if ((index === 0) !== (sw.appShown || sw.atHome))
                App.commands.trigger("view.toggle-app")
        }
    }

    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered && ToolTip.text !== ""
    ToolTip.delay: sw.look.tooltipDelay
    ToolTip.text: sw.plainGraph ? qsTr("This workflow is not an app yet: pick its inputs and outputs (Ctrl+Alt+M)") : ""
}
