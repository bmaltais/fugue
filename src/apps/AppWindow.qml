pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Fugue
import ".."
import "../canvas"
import "../workflow"

// An app as its own application (#398, epic #397, spec 12.18): `fugue --app <id|file>` opens this instead of Main.qml. The window
// is the App view and nothing else: no menu bar, no graph, no node library, no panels, no tabs. Its title is the app's own
// (the window's icon is set from C++ with the app's preview). One notice line at the foot says what the workflow lacks, with
// Get ready… for it, which opens the same sheet the full window has (#379).
//
// Closing it never asks about unsaved work: there is none to lose, the workflow is the app's and is not edited here. Runs the
// window queued stay in the one queue and the helper stays warm; they are not this window's to stop.
ApplicationWindow {
    id: win
    objectName: "appWindow"
    width: 1000
    height: 680
    minimumWidth: 640
    minimumHeight: 440
    visible: true
    title: App.appView.title !== "" ? App.appView.title : qsTr("Fugue")
    // The window's outline is the rounded card inside it, as the owner's Drift window was: the window itself is transparent and the card
    // takes the Corners parameter's card radius. Without an alpha surface (the software renderer) or in the Flat panes layout the card
    // is square and fills the window.
    color: "transparent"
    font.pixelSize: App.theme.textSize

    Material.theme: App.theme.light ? Material.Light : Material.Dark
    Material.accent: App.theme.accent
    Material.background: App.theme.background
    Material.foreground: App.theme.foreground

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit

    readonly property bool roundedCard: GraphicsInfo.api !== GraphicsInfo.Software && look.params.areaLayout !== "panes"
    readonly property real cardRadius: roundedCard ? Math.min(look.params.cornerCard * u, 0.25 * Math.min(width, height)) : 0

    Rectangle {
        id: card
        objectName: "appWindowCard"
        anchors.fill: parent
        radius: win.cardRadius
        color: App.theme.background
        border.width: win.roundedCard && win.look.params.borders === "hairline" ? 1 : 0
        border.color: win.look.hairline
        clip: true

    // Not an app (the file has no block, or one that does not read): the window says so and offers the full window.
    Item {
        anchors { fill: parent; bottomMargin: notice.height }
        AppPage {
            anchors.fill: parent
            standalone: true
            visible: App.appView.isApp
        }
        ColumnLayout {
            anchors.centerIn: parent
            visible: !App.appView.isApp
            spacing: 3 * win.u
            Text {
                objectName: "appNotAnApp"
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("This is not an app.")
                color: win.look.text
                font.pixelSize: win.look.overlayTitleSize
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("It has no simple screen to show. Open it in Fugue to work on it.")
                color: win.look.label
                font.pixelSize: win.look.textSize
            }
            ViewButton {
                objectName: "appNotAnAppOpen"
                Layout.alignment: Qt.AlignHCenter
                look: win.look
                text: qsTr("Open in Fugue")
                glyph: "workflow"
                primary: true
                visible: App.appLauncher !== null && App.appLauncher.available
                onTapped: App.appLauncher.openInFugue()
            }
        }
    }

    Rectangle {
        id: notice
        objectName: "appNotice"
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: visible ? 8 * win.u : 0
        visible: App.workflows.notice.length > 0
        color: win.look.popup
        topLeftRadius: 0
        topRightRadius: 0
        bottomLeftRadius: win.cardRadius
        bottomRightRadius: win.cardRadius
        RowLayout {
            anchors { fill: parent; leftMargin: 4 * win.u; rightMargin: 4 * win.u }
            spacing: 3 * win.u
            Text {
                Layout.fillWidth: true
                text: App.workflows.notice
                color: win.look.text
                font.pixelSize: win.look.labelSize
                elide: Text.ElideRight
            }
            // One button for everything the workflow lacks: packs, their trust and environments, and models (#379).
            ViewButton {
                objectName: "appGetReady"
                look: win.look
                text: qsTr("Get ready…")
                primary: true
                visible: App.workflows.getReadyOffer
                onTapped: App.workflows.readiness.show()
            }
            ViewButton {
                objectName: "appNoticeDismiss"
                look: win.look
                glyph: "x"
                flat: true
                tip: qsTr("Dismiss")
                onTapped: App.workflows.dismissNotice()
            }
        }
    }

    } // card

    // The shortcuts of the command registry (Ctrl+Enter runs the app) and the keys help.
    CommandUi { anchors.fill: parent }

    // The Get ready sheet, made the first time it opens, and kept: the window keeps few items (#5).
    property var getReadySheet: null
    Component {
        id: getReadyComponent
        GetReadySheet {}
    }
    Connections {
        target: App.workflows.readiness
        function onChanged() {
            if (App.workflows.readiness.open && !win.getReadySheet)
                win.getReadySheet = getReadyComponent.createObject(win.contentItem)
        }
    }
}
