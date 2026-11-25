pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../kit"

// The App view (#385, epic #397, spec 12.17): a workflow as one simple screen, in the layout of ComfyUI App Mode (the layout
// and behaviour only, from its docs and screenshots) and the look of the Drift kit (docs/canvas-look.md, epic #456). AppView
// (apps/app_view.h) holds all the logic; this draws it. Three rounded cards sit on the window's background, 2u apart:
//
//   rail    at the start edge (AppRail): Apps (all apps, to pick another: first, the way back to the list), Results and Graph
//           (Open in Graph, the one way out to the nodes; not in an app's own window, `standalone`, whose quiet way out is
//           Open in Fugue in the panel's header)
//   stage   wide, next to the rail (AppStage): the result with zoom and pan, its toolbar, the size line and the takes
//   panel   at the end, about a quarter of the window (80u to 40%): a KitHero header with the app's preview picture (the Dusk
//           gradient without one), its serif title and one-line description, the launcher pin and Open in Fugue as small round
//           buttons in its corner; the inputs as KitGroup rows (AppInput) scrolling; Advanced a quiet disclosure row at the foot
//           of the group; and Run, the screen's one primary control, a full-width accent pill pinned at the panel's foot
//
// Keys: Tab goes through the panel in reading order (hero buttons, inputs, Advanced, Run) with the kit's focus ring on each;
// Enter or Space press the focused button, Ctrl+Enter runs from anywhere (also a prompt), Escape closes a menu or popover.
// Right to left the rows mirror: the rail and stage go to the end side and the panel to the start; a picture is never mirrored.
// "Number of runs" (ComfyUI's stepper) is not here: a tab holds one run at a time, and queuing several of the same bound
// workflow is the run service's, not this page's.
//
// Rules met: docs/ui-ergonomics.md, "App view" row.
Rectangle {
    id: page
    objectName: "appPage"
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property AppView appView: App.appView
    readonly property AppLauncher launcher: App.appLauncher
    readonly property bool launchable: launcher !== null && launcher.available
    // The app's own window (apps/AppWindow.qml): the page is the whole window and has no graph behind it.
    property bool standalone: false
    property bool advancedOpen: false
    readonly property var visibleFields: appView.fields.filter(f => !f.advanced)
    readonly property var advancedFields: appView.fields.filter(f => f.advanced)
    // The form's width: a quarter of the window, at least 80u (320px at the default size), at most 40%.
    readonly property real panelWidth: Math.min(0.4 * width, Math.max(80 * u, 0.25 * width))

    // The gap between the cards: the window's background shows through it.
    readonly property real gap: 2 * u
    color: "transparent"

    // The app being another one: Advanced starts closed again.
    Connections {
        target: page.appView
        function onAppChanged() { page.advancedOpen = false }
    }

    // The Apps dialog (#388): the shelf over this page; choosing a card opens that app as a new tab (the app's own window shows it
    // in place). Made the first time the rail asks.
    property var appsDialog: null
    Component {
        id: appsComponent
        AppsDialog {}
    }
    // The command the key and File > All apps… run: in the full window Main.qml opens the dialog; the app's own window has no
    // Main.qml, so the page opens it here.
    Connections {
        target: App.commands
        enabled: page.standalone
        function onWindowRequested(id) { if (id === "app.show-all") page.showApps() }
    }
    function showApps() {
        if (!appsDialog)
            appsDialog = appsComponent.createObject(page)
        appsDialog.open()
    }

    // A picture dropped anywhere on the page goes into the app's picture input (the last focused, else the first); a picture
    // input's own well, above this, takes it first.
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: event => {
            if (event.hasUrls)
                page.appView.dropImageFiles(event.urls)
        }
    }
    RowLayout {
        anchors { fill: parent; margins: page.gap }
        spacing: page.gap

        AppRail {
            Layout.fillHeight: true
            look: page.look
            standalone: page.standalone
            onAppsRequested: page.standalone ? page.showApps() : App.commands.trigger("app.show-all")
            onResultsRequested: library.open()
            onGraphRequested: App.commands.trigger("view.toggle-app")
        }

        // The stage card: the result well and its controls inside.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: page.look.popupCorner
            color: page.look.popup
            border.width: 1
            border.color: page.look.hairline
            AppStage {
                anchors { fill: parent; margins: 3 * page.u }
                appView: page.appView
                look: page.look
            }
        }

        // The input panel card.
        Rectangle {
            id: panel
            objectName: "appPanel"
            Layout.fillHeight: true
            Layout.preferredWidth: page.panelWidth
            radius: page.look.popupCorner
            color: page.look.popup
            border.width: 1
            border.color: page.look.hairline
            ColumnLayout {
                anchors { fill: parent; margins: 1 }
                spacing: 0

                KitHero {
                    id: hero
                    objectName: "appTitle"
                    Layout.fillWidth: true
                    look: page.look
                    source: page.appView.preview
                    title: page.appView.title
                    subtitle: page.appView.description
                    cornerRadius: page.look.popupCorner
                    ToolTip.visible: heroHover.hovered && page.appView.description !== ""
                    ToolTip.delay: page.look.tooltipDelay
                    ToolTip.text: page.appView.description
                    HoverHandler { id: heroHover }
                    HeroButton {
                        objectName: "appLauncherToggle"
                        visible: page.launchable
                        glyph: "pin"
                        tip: page.launcher && page.launcher.listed ? qsTr("Remove from launcher: take this app out of the desktop launcher")
                                                                   : qsTr("Add to launcher: start this app from the desktop launcher, in a window of its own")
                        onTapped: page.launcher.listed ? page.launcher.remove() : page.launcher.add()
                    }
                    HeroButton {
                        objectName: "appOpenInFugue"
                        visible: page.standalone && page.launchable
                        glyph: "workflow"
                        tip: qsTr("Open in Fugue: the workflow behind this app, in the full window")
                        onTapped: page.launcher.openInFugue()
                    }
                }

                ScrollView {
                    id: scroll
                    objectName: "appForm"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 3 * page.u
                    Layout.topMargin: 0
                    contentWidth: availableWidth
                    clip: true
                    Column {
                        width: scroll.availableWidth
                        KitSection {
                            look: page.look
                            text: qsTr("Inputs")
                            visible: page.visibleFields.length > 0
                        }
                        KitGroup {
                            look: page.look
                            visible: page.visibleFields.length > 0 || page.advancedFields.length > 0
                            Repeater {
                                model: page.visibleFields
                                delegate: AppInput {
                                    required property var modelData
                                    field: modelData
                                    onEdited: (key, value) => page.appView.setValue(key, value)
                                }
                            }
                            KitRow {
                                look: page.look
                                visible: page.advancedFields.length > 0
                                label: qsTr("Advanced")
                                target: advancedChip
                                KitChip {
                                    id: advancedChip
                                    objectName: "appAdvanced"
                                    look: page.look
                                    pill: true
                                    glyph: page.advancedOpen ? "chevron-up" : "chevron-down"
                                    tip: page.advancedOpen ? qsTr("Hide the advanced inputs") : qsTr("Show the advanced inputs")
                                    current: page.advancedOpen
                                    function toggle() { tapped() }
                                    onTapped: page.advancedOpen = !page.advancedOpen
                                }
                            }
                            Repeater {
                                model: page.advancedOpen ? page.advancedFields : []
                                delegate: AppInput {
                                    required property var modelData
                                    field: modelData
                                    onEdited: (key, value) => page.appView.setValue(key, value)
                                }
                            }
                        }
                    }
                }

                Text {
                    objectName: "appProblem"
                    Layout.fillWidth: true
                    Layout.leftMargin: 3 * page.u
                    Layout.rightMargin: 3 * page.u
                    visible: text !== ""
                    text: page.appView.problem
                    color: page.look.error
                    font.pixelSize: page.look.labelSize
                    wrapMode: Text.Wrap
                }
                // Run: the screen's one primary control, a full-width pill in the accent.
                Rectangle {
                    id: run
                    objectName: "appRun"
                    Layout.fillWidth: true
                    Layout.margins: 3 * page.u
                    Layout.preferredHeight: 9 * page.u
                    radius: height / 2
                    color: !enabled ? Qt.alpha(page.look.accent, 0.35) : runTap.pressed ? Qt.darker(page.look.accent, 1.15)
                         : runHover.hovered ? Qt.lighter(page.look.accent, 1.08) : page.look.accent
                    border.width: activeFocus ? 2 : 0
                    border.color: page.look.accentForeground
                    enabled: page.appView.canRun
                    activeFocusOnTab: enabled
                    property string tip: qsTr("Run (Ctrl+Enter)")
                    signal tapped
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Run")
                    Accessible.onPressAction: run.tapped()
                    Keys.onReturnPressed: run.tapped()
                    Keys.onEnterPressed: run.tapped()
                    Keys.onSpacePressed: run.tapped()
                    onTapped: page.appView.run()
                    KitRaise { look: page.look; radius: run.radius; visible: run.enabled }
                    Row {
                        anchors.centerIn: parent
                        spacing: 2 * page.u
                        Glyph {
                            anchors.verticalCenter: parent.verticalCenter
                            look: page.look
                            name: "play"
                            color: page.look.accentForeground
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Run")
                            color: page.look.accentForeground
                            font.pixelSize: Math.round(page.look.textSize * 7 / 6)
                            font.weight: Font.DemiBold
                        }
                    }
                    TapHandler { id: runTap; gesturePolicy: TapHandler.WithinBounds; onTapped: run.tapped() }
                    HoverHandler { id: runHover; cursorShape: Qt.PointingHandCursor }
                    ToolTip.visible: runHover.hovered
                    ToolTip.delay: page.look.tooltipDelay
                    ToolTip.text: tip
                }
            }
        }
    }
    // A small round frosted button over the hero's picture (KitChip frosted: the picture blurred behind it).
    component HeroButton: KitChip {
        look: page.look
        round: true
        frosted: true
        backdrop: hero.backdropItem
        backdropSource: hero.source
    }
    AppLibrary {
        id: library
        appView: page.appView
        look: page.look
    }
}
