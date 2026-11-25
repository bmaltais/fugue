pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../kit"
import "../workspace"

// The App view (#385, epic #397, spec 12.17): a workflow as one simple screen, in the layout of ComfyUI App Mode (the layout
// and behaviour only, from its docs and screenshots) and the look of the Drift kit (docs/canvas-look.md, epic #456). AppView
// (apps/app_view.h) holds all the logic; this draws it. Three rounded cards sit on the window's background, 2u apart:
//
//   rail    at the start edge (AppRail): Apps (all apps, to pick another: first, the way back to the list), Results and Graph
//           (Open in Graph, the one way out to the nodes; not in an app's own window, `standalone`, whose quiet way out is
//           Open in Fugue in the panel's header)
//   stage   wide, next to the rail (AppStage): the result with zoom and pan, its toolbar, the size line and the takes
//   panel   at the end, about a quarter of the window (80u to 40%) until a person drags its border (then that width, kept between
//           runs: App.sidePanels.appWidth); dragged under half its least width, or its hero's hide button, or Window > App inputs, it
//           slides shut to a slim strip with Show the inputs and Run (shown or not is the registry's "app-inputs", kept between runs).
//           It is a KitHero header with the app's preview picture (the Dusk
//           gradient without one), its serif title and one-line description, the launcher pin and Open in Fugue as small round
//           buttons in its corner; the inputs as KitGroup rows (AppInput) scrolling; Advanced a quiet disclosure row at the foot
//           of the group; and Run, the screen's one primary control, a full-width accent pill pinned at the panel's foot
//
// Keys: Tab goes through the panel in reading order (hero buttons, inputs, Advanced, Run again (arrows choose), Run; Space flips a switch) with the kit's focus ring on each;
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
    // The form's width. Automatic: a quarter of the window, at least 80u (320px at the default size), at most 40%. Once its border
    // was dragged: that width, kept between 80u (less in a narrow area) and what leaves the stage 100u.
    readonly property real autoPanelWidth: Math.min(0.4 * width, Math.max(80 * u, 0.25 * width))
    readonly property real maxPanelWidth: Math.max(autoPanelWidth, width - 2 * gap - 14 * u - 2 * gap - 100 * u)
    readonly property real minPanelWidth: Math.min(80 * u, autoPanelWidth)
    property real keptHere: 0 // the dragged width when there is no App.sidePanels to keep it
    readonly property real chosenPanelWidth: App.sidePanels !== null ? App.sidePanels.appWidth : keptHere
    readonly property real panelWidth: chosenPanelWidth > 0 ? Math.max(minPanelWidth, Math.min(maxPanelWidth, chosenPanelWidth)) : autoPanelWidth
    function choosePanelWidth(wanted) {
        const w = wanted > 0 ? Math.round(Math.max(minPanelWidth, Math.min(maxPanelWidth, wanted))) : 0
        if (App.sidePanels !== null)
            App.sidePanels.appWidth = w
        else
            keptHere = w
    }

    // The inputs panel shown, or slid shut to the strip (owner, 2026-11-25: "a way to hide it with a UI element to bring it back").
    // The registry keeps it (Window > App inputs, focus mode); the app's own window has none, so it is the page's there.
    readonly property bool inputsInRegistry: App.chrome !== null && App.chrome.shown["app-inputs"] !== undefined
    property bool hiddenHere: false
    readonly property bool inputsShown: inputsInRegistry ? App.chrome.shown["app-inputs"] : !hiddenHere
    function showInputs(on) {
        if (on === inputsShown)
            return
        const hadFocus = hideInputs.activeFocus || showInputsChip.activeFocus
        if (inputsInRegistry)
            App.commands.trigger("view.toggle-app-inputs")
        else
            hiddenHere = !on
        if (hadFocus) // the keyboard's place follows to the button that brings it back
            Qt.callLater(() => (page.inputsShown ? hideInputs : showInputsChip).forceActiveFocus(Qt.TabFocusReason))
    }
    // The strip the panel slides shut to: one 7u chip and 1u each side.
    readonly property real stripWidth: 9 * u
    // 1 while the form shows in full, 0 shut: it slides (look.params.durThumb, 0 under reduced motion) when the panel is shown or
    // hidden, never while the page is being made; the end card's width follows it, so a drag or a resized window is followed at once.
    property real openness: inputsShown ? 1 : 0
    property bool made: false
    Component.onCompleted: Qt.callLater(() => page.made = true)
    Behavior on openness {
        enabled: page.made && page.visible
        NumberAnimation { duration: page.look.params.durThumb; easing.type: Easing.OutCubic }
    }
    readonly property real endWidth: stripWidth + Math.max(0, panelWidth - stripWidth) * openness

    // The gap between the cards: the window's background shows through it.
    readonly property real gap: look.params.areaLayout === "panes" ? 0 : look.params.areaGap * u
    // The tone of the app's header (Dusk by default; an app may later pick one).
    readonly property string tone: "dusk"
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
        KitPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            look: page.look
            tone: page.tone
            calm: true
            AppStage {
                anchors { fill: parent; margins: 3 * page.u }
                appView: page.appView
                look: page.look
            }
        }

        // The input panel card. Its content keeps the panel's width and is clipped while the card slides shut, so it moves with the
        // card's edge instead of squeezing.
        KitPanel {
            id: panel
            objectName: "appPanel"
            Layout.fillHeight: true
            Layout.preferredWidth: page.endWidth
            look: page.look
            tone: page.tone
            clip: page.openness < 1

            // Shut: a slim strip with the way back and Run (Ctrl+Enter runs too).
            Column {
                objectName: "appInputsStrip"
                anchors { top: parent.top; topMargin: page.u; horizontalCenter: parent.horizontalCenter }
                spacing: page.u
                visible: !page.inputsShown
                opacity: 1 - page.openness
                KitChip {
                    id: showInputsChip
                    objectName: "appShowInputs"
                    look: page.look
                    round: true
                    glyph: LayoutMirroring.enabled ? "chevron-right" : "chevron-left"
                    tip: qsTr("Show the inputs")
                    Accessible.name: qsTr("Show the inputs")
                    onTapped: page.showInputs(true)
                }
            }
            KitChip {
                objectName: "appStripRun"
                anchors { bottom: parent.bottom; bottomMargin: page.u; horizontalCenter: parent.horizontalCenter }
                visible: !page.inputsShown
                opacity: 1 - page.openness
                look: page.look
                round: true
                primary: true
                glyph: "play"
                enabled: page.appView.canRun
                tip: qsTr("Run (Ctrl+Enter)")
                Accessible.name: qsTr("Run")
                onTapped: page.appView.run()
            }

            ColumnLayout {
                anchors { top: parent.top; bottom: parent.bottom; left: parent.left; margins: 1 }
                width: page.panelWidth - 2
                spacing: 0
                visible: page.inputsShown || page.openness > 0
                opacity: page.openness

                KitHero {
                    id: hero
                    objectName: "appTitle"
                    Layout.fillWidth: true
                    look: page.look
                    source: page.appView.preview
                    video: page.appView.previewVideo
                    videoActive: page.visible // KitVideo adds the window's focus; the poster stays under reduced motion
                    title: page.appView.title
                    subtitle: page.appView.description
                    tone: page.tone
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
                        objectName: "appKeep"
                        visible: page.launcher !== null && page.launcher.keepable
                        glyph: "plus"
                        tip: qsTr("Keep this app: add it to your apps and your launcher")
                        onTapped: page.launcher.keep()
                    }
                    HeroButton {
                        id: hideInputs
                        objectName: "appHideInputs"
                        glyph: LayoutMirroring.enabled ? "chevron-left" : "chevron-right"
                        tip: qsTr("Hide the inputs: more room for the result")
                        onTapped: page.showInputs(false)
                    }
                    HeroButton {
                        objectName: "appOpenInFugue"
                        visible: page.standalone && page.launchable
                        glyph: "workflow"
                        tip: qsTr("Open in Fugue: the workflow behind this app, in the full window")
                        onTapped: page.launcher.openInFugue()
                    }
                }

                // What the app still needs, while it is not ready (#469): a quiet block above the inputs, clear when ready.
                NeedsPanel {
                    id: needsPanel
                    Layout.fillWidth: true
                    Layout.margins: page.look.params.panelPad * page.u
                    Layout.topMargin: page.look.params.panelPad * page.u
                    Layout.bottomMargin: 0
                    look: page.look
                    needs: page.appView.needs
                    standalone: page.standalone
                    maxHeight: 0.38 * panel.height
                }

                ScrollView {
                    id: scroll
                    objectName: "appForm"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: page.look.params.panelPad * page.u
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
                    Layout.leftMargin: page.look.params.panelPad * page.u
                    Layout.rightMargin: page.look.params.panelPad * page.u
                    visible: text !== ""
                    text: page.appView.problem
                    color: page.look.error
                    font.pixelSize: page.look.labelSize
                    wrapMode: Text.Wrap
                }
                // Live (#471): off at first, per tab, for the session. Live re-runs once an input has been still for a moment (the
                // canvas Live toggle's wording, #315); Repeat starts the next run as soon as one ends (with a Random seed: new
                // pictures one after another). One at a time; Cancel ends either. Nothing runs, and nothing is armed, while both are off.
                KitGroup {
                    id: liveGroup
                    objectName: "appLive"
                    Layout.fillWidth: true
                    Layout.leftMargin: page.look.params.panelPad * page.u
                    Layout.rightMargin: page.look.params.panelPad * page.u
                    look: page.look
                    visible: page.appView.problem === ""
                    // One mode, one row (coordinator, overnight: two switches for one three-way mode read as two settings
                    // and cost a row of the form): Off, On change (Live), Repeat.
                    KitRow {
                        look: page.look
                        label: qsTr("Run again")
                        KitSegmented {
                            id: liveChooser
                            objectName: "appLiveMode"
                            look: page.look
                            text: qsTr("Run again")
                            // Repeat only for an app with a seed: with none every run is the same (AppView.canRepeat).
                            readonly property var modes: page.appView.canRepeat ? ["off", "live", "repeat"] : ["off", "live"]
                            options: page.appView.canRepeat ? [qsTr("Off"), qsTr("On change"), qsTr("Repeat")] : [qsTr("Off"), qsTr("On change")]
                            currentIndex: Math.max(0, modes.indexOf(page.appView.liveMode))
                            onMoved: index => page.appView.setLiveMode(modes[index])
                        }
                    }
                }
                // Repeat chosen while no seed varies: the next run would be the same picture, so it waits and says why.
                Text {
                    objectName: "appRepeatWaits"
                    Layout.fillWidth: true
                    Layout.leftMargin: page.look.params.panelPad * page.u
                    Layout.rightMargin: page.look.params.panelPad * page.u
                    Layout.topMargin: page.u
                    visible: page.appView.repeatWaits && page.appView.problem === ""
                    text: qsTr("Repeat waits: set a seed to Random or Increment, or each run makes the same picture.")
                    wrapMode: Text.Wrap
                    color: page.look.label
                    font.pixelSize: page.look.labelSize
                }
                // Where the app runs, when not here (#395): a quiet line above Run, nothing for a local run.
                Text {
                    objectName: "appRunsOn"
                    Layout.fillWidth: true
                    Layout.leftMargin: page.look.params.panelPad * page.u
                    Layout.rightMargin: page.look.params.panelPad * page.u
                    visible: page.appView.runsOn !== ""
                    text: qsTr("Runs on %1").arg(page.appView.runsOn)
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: page.look.label
                    font.pixelSize: page.look.labelSize
                    Accessible.name: text
                }
                // Run: the screen's one primary control, a full-width pill in the accent.
                Rectangle {
                    id: run
                    objectName: "appRun"
                    Layout.fillWidth: true
                    Layout.margins: page.look.params.panelPad * page.u
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
    // The border between the stage and the inputs panel, over the gap between the cards (Blender's region edge: drag to resize, drag
    // under half the least width to hide; the area border's handle, so it looks and answers alike). Enter or a double click gives the
    // automatic width back.
    AreaHandle {
        id: panelHandle
        objectName: "appPanelHandle"
        visible: page.inputsShown && page.openness >= 1
        look: page.look
        axis: "row"
        rightToLeft: LayoutMirroring.enabled
        readonly property real border: rightToLeft ? panel.x + panel.width + page.gap / 2 : panel.x - page.gap / 2
        x: border - width / 2
        y: panel.y
        width: thickness
        height: panel.height
        z: 2
        name: qsTr("Border of the inputs panel")
        description: qsTr("Arrow keys resize it; Enter gives the automatic width")
        tip: qsTr("Drag to resize; drag to the edge to hide; double-click for the automatic width")
        onMoveRequested: delta => {
            const wanted = page.panelWidth - delta
            if (wanted < 0.5 * page.minPanelWidth)
                page.showInputs(false)
            else
                page.choosePanelWidth(Math.max(wanted, page.minPanelWidth))
        }
        onEqualizeRequested: page.choosePanelWidth(0)
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
