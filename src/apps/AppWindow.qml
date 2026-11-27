pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Fugue
import ".."
import "../canvas"
import "../kit"
import "../workflow"
import "../run"
import "../settings"

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
        anchors { fill: parent; bottomMargin: notice.visible ? notice.height + 4 * win.u : 0 }
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

    // The window's one notice (docs/canvas-look.md 20.23): what the workflow lacks with Get ready…, or the files "Use my file" put in
    // place, one line each, with Undo. The same KitNotice as the full window's banner, a card over the foot of the page.
    KitNotice {
        id: notice
        objectName: "appNotice"
        readonly property bool swapped: App.workflows.noticeLines.length > 0
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 2 * win.u }
        visible: App.workflows.notice.length > 0
        look: win.look
        tone: swapped ? "attention" : App.workflows.getReadyOffer ? "info" : "neutral"
        title: swapped ? App.workflows.noticeTitle : App.workflows.notice
        lines: App.workflows.noticeLines
        copyText: App.workflows.notice
        announceChanges: !App.workflows.readingNotice // "Reading your file… 42%" is said once, not at every percent
        onDismissed: App.workflows.dismissNotice()
        actions: Component {
            Row {
                spacing: 2 * win.u
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
                    objectName: "appNoticeUndo"
                    look: win.look
                    text: qsTr("Undo")
                    visible: App.workflows.noticeUndoable
                    onTapped: App.workflows.undoNotice()
                }
            }
        }
    }

    } // card

    // The shortcuts of the command registry (Ctrl+Enter runs the app) and the keys help.
    CommandUi { anchors.fill: parent }

    // The question a run waits on before it sends data off this machine (#404).
    RemoteConsentQuestion {}

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
    // Get ready's "Find model folders…" (#619): the setup sheet, made the first time it opens.
    property var modelSetupSheet: null
    Component {
        id: modelSetupComponent
        ModelSetupSheet {}
    }
    Connections {
        target: App.settings ? App.settings.modelSetup : null
        function onChanged() {
            if (App.settings.modelSetup.open && !win.modelSetupSheet)
                win.modelSetupSheet = modelSetupComponent.createObject(win.contentItem)
        }
    }
}
