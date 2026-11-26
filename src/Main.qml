pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Fugue
import "apps"
import "canvas"
import "menu"
import "panels"
import "run"
import "search"
import "settings"
import "workflow"
import "workspace"

ApplicationWindow {
    id: win
    width: 960
    // 680 for the canvas and the menu bar's 7u at the default text size.
    height: 708
    minimumWidth: 640
    minimumHeight: 460
    visible: true
    title: App.workflows.windowTitle
    color: App.theme.background
    font.pixelSize: App.theme.textSize

    Material.theme: App.theme.light ? Material.Light : Material.Dark
    Material.accent: App.theme.accent
    Material.background: App.theme.background
    Material.foreground: App.theme.foreground
    // The window manager's close (Super+W, the title bar's x) asks about unsaved work as Quit does (spec 11.3).
    onClosing: close => close.accepted = App.commands.mayCloseWindow()

    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    // Rounded spaces with gaps (docs/canvas-look.md 20.1, 20.21): the canvas or the areas, and a side panel, are cards a gap of window
    // background apart, half of it on each side of a border and half between a card and the window's edge; the Panes layout has none.
    readonly property real cardGap: App.theme.look.params.areaLayout === "panes" ? 0 : App.theme.look.params.areaGap * App.theme.look.unit
    readonly property real cardHalf: cardGap / 2
    // The cards' top and bottom in the main column: the Graph's header above the canvas is part of its card.
    // The cards' span (the side strip and panels use it): the shelf's own while it shows. The canvas under it is not laid out then,
    // so measuring the canvas left the strip short of the window's foot (owner, 2026-11-25).
    readonly property Item cardItem: homeItem ?? body
    readonly property real cardTop: graphHeader.active ? graphHeader.y : cardItem.y
    readonly property real cardBottom: cardItem.y + cardItem.height
    // The settings (settings/settings_model.h); a test window may run without them.
    readonly property SettingsModel appSettingsModel: App.settings
    // The shown workflow has an app block (#385), and the window shows it as the App view.
    readonly property bool appIsApp: App.appView ? App.appView.isApp : false
    // One area only: with several, the areas are the window's body and an area whose view type is App shows it (apps/AppArea.qml, #450).
    readonly property bool appShown: (App.appView ? App.appView.shown : false) && !areaHost
    // Nothing is open and the person has not asked for a blank graph: the apps shelf is the body (#388).
    readonly property bool homeShown: (App.appShelf ? App.appShelf.atHome : false) && !appShown && !areaHost

    HelperNotice { id: helperNotice }
    // The width, place and delay of every tooltip in the window (canvas/ToolTipStyle.qml).
    ToolTipStyle { look: App.theme.look; windowWidth: win.width }

    // Which bars and panels are shown (#129, panels/chrome_visibility.h). A window without the
    // registry (a test window) shows them all.
    readonly property bool focusMode: App.chrome !== null && App.chrome.focusMode
    function chromeIsShown(id) {
        return App.chrome === null || App.chrome.shown[id] !== false
    }

    // One line that says how to get chrome back, for a few seconds: made when needed, dropped after.
    property Item chromeHintItem: null
    function showChromeHint(text) {
        if (chromeHintItem)
            chromeHintItem.destroy()
        chromeHintItem = chromeHintComponent.createObject(windowRoot, {text: text}) as Item
    }
    Connections {
        target: App.chrome
        function onFocusModeChanged() {
            if (App.chrome.focusMode)
                win.showChromeHint(qsTr("Focus mode. H brings everything back."))
        }
    }
    Component {
        id: chromeHintComponent
        Rectangle {
            id: hint
            objectName: "chromeHint"
            property string text
            z: 20
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 4 * App.theme.look.unit
            width: hintLabel.implicitWidth + 6 * App.theme.look.unit
            height: 7 * App.theme.look.unit
            radius: Math.min(App.theme.look.params.cornerCard * App.theme.look.unit, height / 2)
            color: App.theme.look.popup
            border.color: App.theme.look.hairline
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
            Label {
                id: hintLabel
                anchors.centerIn: parent
                color: App.theme.look.text
                text: hint.text
            }
            Timer {
                running: true
                interval: 4000
                onTriggered: hint.destroy()
            }
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        id: windowRoot
        anchors.fill: parent
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        // Escape slides an auto-hidden menu bar out when nothing else wants the key (the canvas
        // and the fields take it first).
        Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape && menuBarHost.escapeWanted
        Keys.onEscapePressed: event => {
            if (menuBarHost.escapeWanted) {
                menuBarHost.conceal()
                event.accepted = true
            }
        }

        // The window bar (its menus and, in its row, the workflow tabs) hides from View > Bars and panels (#129; items are kept, not dropped:
        // the window's item count is a budget and a Loader each would add to it). Not shown, the
        // menu bar auto-hides (#160): a slim edge and a handle stay, and it slides in over the canvas
        // for the pointer at the edge, F10 and the Alt keys (panels/AutoHide.qml). It sits above the
        // layout, which starts under it, so it also slides over the side strip.
        AutoHide {
            id: menuBarHost
            anchors { top: parent.top; left: parent.left; right: parent.right }
            z: 5 // over the side strip and panel, which the bar slides across
            pinned: win.chromeIsShown("menu-bar")
            edgeShown: !win.focusMode
            handleInset: win.rightToLeft ? 0 : win.body.x
            holdOpen: menuBar.anyShown
            handleName: qsTr("Show the menu bar")
            MenuBar {
                id: menuBar
                width: parent.width
                tabsAllowed: win.chromeIsShown("workflow-tabs")
                onNeeded: menuBarHost.reveal()
            }
        }
        ColumnLayout {
            id: mainLayout
            anchors { fill: parent; topMargin: menuBarHost.implicitHeight }
            spacing: 0

            // Faster generation on an NVIDIA card with only the Vulkan helper (#97, spec 8.10).
            // Made only while shown: the window keeps few items (#5).
            Loader {
                Layout.fillWidth: true
                Layout.preferredHeight: item ? (item as Item).implicitHeight : 0
                Layout.margins: active ? 2 * App.theme.look.unit : 0
                active: helperNotice.shown
                sourceComponent: Banner {
                    objectName: "helperNotice"
                    look: App.theme.look
                    tone: "moss" // an offer to get something ready, as Get ready's
                    RowLayout {
                        id: noticeRow
                        width: parent.width
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: App.theme.look.text
                            text: helperNotice.pacman ? qsTr("Faster generation on your NVIDIA card: install fugue-helper-cuda")
                                                      : qsTr("Faster generation on your NVIDIA card: download the CUDA helper")
                        }
                        ThemedButton {
                            objectName: "helperNoticeCopy"
                            highlighted: true
                            text: helperNotice.pacman ? qsTr("Copy %1").arg(helperNotice.command) : qsTr("Copy the install command")
                            onClicked: helperNotice.copy()
                        }
                        ToolButton {
                            objectName: "helperNoticeDismiss"
                            // The Lucide x every other close and dismiss draws (canvas-look 16), not a text "×".
                            contentItem: Glyph { look: App.theme.look; name: "x"; color: App.theme.look.label; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            focusPolicy: Qt.NoFocus
                            Accessible.name: qsTr("Dismiss")
                            onClicked: helperNotice.dismiss()
                        }
                    }
                }
            }
            // The Graph's area header while the window has one area (#262): the same bar every area has, with the Graph's own
            // menus (View, Select, Add, Node). With more areas the host's headers do this and this one goes.
            Loader {
                id: graphHeader
                objectName: "graphHeaderHost"
                Layout.fillWidth: true
                Layout.preferredHeight: item ? 7 * App.theme.look.unit : 0
                Layout.leftMargin: win.stripWidth + win.sidePanelMargin("left") + win.cardHalf
                Layout.rightMargin: win.sidePanelMargin("right") + win.cardHalf
                Layout.topMargin: active ? win.cardHalf : 0
                active: !win.areaHost && !win.focusMode && !win.appShown && !win.homeShown && App.workspace !== null
                sourceComponent: AreaHeader {
                    areaId: App.workspace.graphArea
                    viewType: "graph"
                    leadingClear: Math.max(0, menuBarHost.handleEnd - win.body.x)
                }
            }
            FugueCanvas {
                id: fugueCanvas
                objectName: "canvas"
                visible: !win.appShown && !win.homeShown && !win.graphHidden
                Layout.fillWidth: true
                Layout.fillHeight: true
                // The side panel (#56) takes its width from the canvas, on the side it docks.
                Layout.leftMargin: win.stripWidth + win.sidePanelMargin("left") + win.cardHalf
                Layout.rightMargin: win.sidePanelMargin("right") + win.cardHalf
                Layout.topMargin: graphHeader.active ? 0 : win.cardHalf
                Layout.bottomMargin: win.cardHalf
                model: App.canvasModel
                floatingBar: (win.runToolbarItem as RunToolbar)?.surface ?? null
                navigation: win.appSettingsModel ? win.appSettingsModel.navigation : "legacy"
                hoverMagnifier: win.appSettingsModel ? win.appSettingsModel.hoverMagnifier : false
                // The auto-hide menu bar's handle hangs over the canvas's top left (#160); over the top start
                // area's header while the window has areas.
                leadingClear: win.areaHost || graphHeader.active ? 0 : Math.max(0, menuBarHost.handleEnd - x)
                selectionToolbarShown: win.chromeIsShown("selection-toolbar")
                breadcrumbShown: win.chromeIsShown("breadcrumb")
                runPillShown: win.chromeIsShown("run-status")
                zoomPillShown: win.chromeIsShown("zoom-readout")
                onSearchRequested: (x, y) => App.canvasModel.search.openAt(x, y)
                onLinkReleased: (from, x, y) => App.canvasModel.search.openFromLink(from, x, y)
                onContextMenuRequested: (x, y) => contextMenu.openAt(x, y, false)
                onMenuRequested: (kind, x, y) => contextMenu.openMenu(kind, x, y)

                // Tab and Shift+A (`node.search`) open search where the pointer is; Add node
                // in the context menu, where it was opened.
                Connections {
                    target: App.canvasModel.search
                    function onPointerSearchRequested() {
                        const t = App.commands.target()
                        const p = t.x !== undefined ? Qt.point(t.x, t.y) : fugueCanvas.pointerWorld()
                        App.canvasModel.search.openAt(p.x, p.y)
                    }
                }
                ContextMenu {
                    id: contextMenu
                    anchors.fill: parent
                    canvas: fugueCanvas
                }

            }
        }

        // The Graph's card while the window has one area: the corners and the hairline round the header and the canvas.
        AreaFrame {
            objectName: "graphFrame"
            x: fugueCanvas.x
            y: mainLayout.y + win.cardTop
            width: fugueCanvas.width
            height: win.cardBottom - win.cardTop
            shown: !win.areaHost && !win.appShown && !win.homeShown
        }

        // The file pickers, the save and trust and recovery questions, the drop area.
        WorkflowDialogs { anchors.fill: parent; canvasView: fugueCanvas; noticeInset: win.stripWidth }
        GroupDialogs { anchors.fill: parent }
        RemoteConsentQuestion {}
        AppBuilderDialog { anchors.fill: parent }
        LayoutColorPicker {}
        ImageSaveDialog {}
    }

    // The command registry's shortcuts, the `?` overlay and the quit question.
    CommandUi { anchors.fill: parent }

    // The window's body is an area tree (#217, spec 12.16; App.workspace, workspace/workspace.h). With one area
    // it shows the Graph: the canvas above, declared in the column as before and adopted as the area's view; an
    // AreaNode is not an item and, with no host, makes and moves nothing, so the window has exactly the items it
    // had (test_area_window). With more than one, the column gets a host for the body in the canvas's place (same
    // margins), the canvas moves into it as its area's view, and the host has the other views, the headers and
    // the border handles; back to one area, the canvas returns to the column and the host goes.
    AreaNode {
        id: areas
        objectName: "areas"
        node: AreaLayout.defaultTree().root
        views: ({"1": fugueCanvas})
        area: win.areaHost ? Qt.rect(0, 0, win.areaHost.width, win.areaHost.height) : Qt.rect(0, 0, 0, 0)
        registry: App.workspace ? App.workspace.views : null
        controller: App.workspace
        rightToLeft: win.rightToLeft
        header: areaHeaderComponent
        handle: areaHandleComponent
        frame: areaFrameComponent
        gap: win.cardGap
        // Focus mode hides the headers with the other bars (12.15); the borders stay.
        headerHeight: win.focusMode ? 0 : 7 * App.theme.look.unit
        leadingClear: Math.max(0, menuBarHost.handleEnd - win.body.x)
    }
    // The App view (#385): a workflow with an app block as one simple screen, in the canvas's place. Made only while needed, as
    // the areas' host is, so any other window has none of these items (test_area_window). The way back to it from the graph is
    // the window bar's App | Graph switch (menu/MenuBar.qml, #397).
    property Item appPageItem: null
    // The apps shelf in the same place while nothing is open (#388), made only then, for the same reason.
    property Item homeItem: null
    Component {
        id: homeComponent
        AppsHome {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: win.stripWidth + win.sidePanelMargin("left")
            Layout.rightMargin: win.sidePanelMargin("right")
        }
    }
    Component {
        id: appPageComponent
        AppPage {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: win.stripWidth + win.sidePanelMargin("left")
            Layout.rightMargin: win.sidePanelMargin("right")
        }
    }
    function syncApp() {
        const page = appShown
        if (page && !appPageItem) {
            appPageItem = appPageComponent.createObject(mainLayout) as Item
        } else if (!page && appPageItem) {
            appPageItem.destroy()
            appPageItem = null
        }
    }
    // What the App view's store needs of the areas (#450): whether the window is split, and whether its active area shows the App.
    function syncAppAreas() {
        if (App.appView)
            App.appView.setAreas(areaHost !== null, App.workspace ? App.workspace.viewOf(App.workspace.active) === "app" : false)
    }
    Connections {
        target: App.workspace
        function onChanged() { win.syncAppAreas() }
        function onActiveChanged() { win.syncAppAreas() }
    }
    // The key, the switch, the rail's Graph and the builder ask the App view for one view or the other; in a split the areas do it.
    Connections {
        target: App.appView
        function onAreaToggleRequested() { App.workspace?.toggleApp() }
        function onAreaViewRequested(app) { App.workspace?.requestApp(app) }
    }
    function syncHome() {
        if (homeShown && !homeItem) {
            homeItem = homeComponent.createObject(mainLayout) as Item
        } else if (!homeShown && homeItem) {
            homeItem.destroy()
            homeItem = null
        }
    }
    onHomeShownChanged: syncHome()
    onAppShownChanged: syncApp()
    onAppIsAppChanged: syncApp()
    onFocusModeChanged: syncApp()
    onAreaHostChanged: syncApp()
    // What the areas sit in: the canvas alone, or the host while there are several.
    property Item areaHost: null
    readonly property Item body: areaHost ?? fugueCanvas
    // No area shows the Graph (Blender's editors: any area can switch its view): it stays alive, hidden, with its run toolbar and
    // context menu, and the commands that act on it are off (the `canvas` context). It re-places itself when an area shows it again.
    readonly property bool graphHidden: areaHost !== null && App.workspace !== null && App.workspace.graphArea === 0
    function syncGraphContext() {
        if (App.commands)
            App.commands.setContextActive("canvas", !graphHidden)
    }
    onGraphHiddenChanged: syncGraphContext()
    function syncAreas() {
        const tree = App.workspace ? App.workspace.tree : AreaLayout.defaultTree()
        // The host is for every layout but the default one: the Graph alone in the one area, drawn by the window's column.
        const split = !(tree.root?.type === "leaf" && tree.root.view === "graph")
        if (split && !areaHost) {
            areaHost = areaHostComponent.createObject(mainLayout) as Item
            fugueCanvas.parent = areaHost
        }
        // The Graph is listed only while an area shows it: with none, no area adopts it and nothing places it.
        const views = {}
        const graphArea = App.workspace ? App.workspace.graphArea : 1
        if (graphArea > 0)
            views[String(graphArea)] = fugueCanvas
        areas.views = views
        areas.host = split ? areaHost : null
        areas.node = tree.root
        if (!split && areaHost) {
            fugueCanvas.parent = mainLayout
            areaHost.destroy()
            areaHost = null
        }
        syncAppAreas()
    }
    // A press in an area makes it the active one (the commands act on it), and gives its view the keys.
    function activateAreaAt(point) {
        const id = areas.areaAt(point.x, point.y)
        if (id <= 0 || !App.workspace)
            return
        App.workspace.active = id
        const view = areas.viewOf(id)
        let focused = win.activeFocusItem
        while (focused && focused !== view)
            focused = focused.parent
        if (view && !focused)
            view.forceActiveFocus()
    }
    // So does the keyboard focus arriving in an area (Tab).
    onActiveFocusItemChanged: {
        if (!areaHost || !App.workspace)
            return
        let item = activeFocusItem
        while (item && item.parent !== areaHost)
            item = item.parent
        const id = item ? areas.areaOfItem(item) : 0
        if (id > 0)
            App.workspace.active = id
    }
    Connections {
        target: App.workspace
        function onChanged() { win.syncAreas() }
    }
    Binding {
        when: App.workspace !== null
        target: App.workspace
        property: "bodySize"
        value: Qt.size(win.body.width, win.body.height)
    }
    Component {
        id: areaHostComponent
        AreaHost {
            objectName: "areaHost"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: win.stripWidth + win.sidePanelMargin("left") + win.cardHalf
            Layout.rightMargin: win.sidePanelMargin("right") + win.cardHalf
            Layout.topMargin: win.cardHalf
            Layout.bottomMargin: win.cardHalf
            onPressed: point => win.activateAreaAt(point)
        }
    }
    Component {
        id: areaHeaderComponent
        AreaHeader {}
    }
    Component {
        id: areaFrameComponent
        AreaFrame {}
    }
    Component {
        id: areaHandleComponent
        AreaHandle { look: App.theme.look }
    }

    // The settings panel (#51), made the first time app.settings (Ctrl+,) asks for it.
    Connections {
        target: App.commands
        function onWindowRequested(id) {
            if (id === "file.templates") {
                if (!win.templatesDialog)
                    win.templatesDialog = templatesComponent.createObject(win.contentItem)
                win.templatesDialog.open()
                return
            }
            if (id === "app.show-all") {
                if (!win.appsDialog)
                    win.appsDialog = appsComponent.createObject(win.contentItem)
                win.appsDialog.open()
                return
            }
            if (id === "file.projects") {
                if (App.projects === null)
                    return
                if (!win.projectsDialog)
                    win.projectsDialog = projectsComponent.createObject(win.contentItem)
                win.projectsDialog.open()
                return
            }
            if ((id !== "app.settings" && id !== "cache.clear-previews") || !win.appSettingsModel)
                return
            if (!win.settingsPanel)
                win.settingsPanel = settingsComponent.createObject(win.contentItem)
            if (id === "cache.clear-previews") {
                // Settings > Cache, asking about the previews: what is lost is said there (#141).
                win.settingsPanel.page = "cache"
                win.appSettingsModel.cache.refresh()
                win.appSettingsModel.cache.ask("thumbnails")
            }
            win.settingsPanel.open()
        }
    }
    property var settingsPanel: null
    // The banner's Install for a workflow's missing pack (#91): Settings > Packs, which shows
    // what it is and asks before it installs and before it is trusted.
    Connections {
        target: App.workflows
        function onPackInstallRequested(source, ref) {
            if (!win.appSettingsModel)
                return
            if (!win.settingsPanel)
                win.settingsPanel = settingsComponent.createObject(win.contentItem)
            win.settingsPanel.page = "packs"
            win.settingsPanel.open()
            win.appSettingsModel.packs.add(source, ref)
        }
    }
    // The Apps dialog (#388), made the first time app.show-all asks for it.
    property var appsDialog: null
    Component {
        id: appsComponent
        AppsDialog {}
    }
    // The Templates dialog (#83), made the first time file.templates asks for it.
    property var templatesDialog: null
    Component {
        id: templatesComponent
        TemplatesDialog {}
    }
    // The Projects dialog (spec 1.4.2), made the first time file.projects (or the project chip) asks for it.
    property var projectsDialog: null
    Component {
        id: projectsComponent
        ProjectsDialog { model: App.projects }
    }
    Component {
        id: settingsComponent
        SettingsPanel { model: win.appSettingsModel }
    }

    // The run toolbar and the queue panel (#60), floating over the canvas. Each is made when it is
    // shown and dropped when it is not, and neither at all in a window without runs: the window
    // keeps few items (#5), and not even a Loader is left standing for them.
    property Item runToolbarItem: null
    property Item runQueueItem: null
    readonly property bool runBarWanted: App.runBar !== null && App.runs !== null && App.runQueue !== null
    function syncRunBar() {
        const toolbar = runBarWanted && App.runBar.visible
        if (toolbar && !runToolbarItem)
            runToolbarItem = runToolbarComponent.createObject(fugueCanvas) as Item
        else if (!toolbar && runToolbarItem) {
            runToolbarItem.destroy()
            runToolbarItem = null
        }
        const panel = toolbar && App.runBar.queueOpen
        if (panel && !runQueueItem) {
            runQueueItem = runQueueComponent.createObject(fugueCanvas) as Item
            App.runQueue.watching = true
        } else if (!panel && runQueueItem) {
            runQueueItem.destroy()
            runQueueItem = null
            App.runQueue.watching = false
        }
    }
    Component.onCompleted: {
        syncRunBar()
        syncSidePanel()
        syncAreas()
        syncGraphContext()
        syncApp()
        syncHome()
    }
    Connections {
        target: App.runBar
        function onVisibleChanged() { win.syncRunBar() }
        function onQueueOpenChanged() { win.syncRunBar() }
        function onQueueFocusRequested() { (win.runQueueItem as RunQueuePanel)?.focusFirst() }
    }
    Component {
        id: runToolbarComponent
        RunToolbar {
            look: App.theme.look
            busy: App.runs.busy
            canCancel: App.runs.busy
            pending: App.runQueue.count
            progress: App.runs.running ? App.runs.progress : -1
            batch: App.runs.batch
            maxBatch: 99
            place: App.runBar.place
            queueOpen: App.runBar.queueOpen
            onMenuAboutToOpen: menuItems = App.commands.menu("run")
            onRunRequested: App.commands.trigger("run.start")
            onCancelRequested: App.commands.trigger("run.cancel")
            onBatchRequested: value => App.runs.batch = value
            onQueueToggled: App.runBar.queueOpen = !App.runBar.queueOpen
            onPlaceRequested: place => App.runBar.place = place
        }
    }
    Component {
        id: runQueueComponent
        RunQueuePanel {
            id: runQueuePanel
            look: App.theme.look
            entries: App.runQueue.entries
            history: App.runQueue.history
            // It hangs from the bar on the side with room (src/engine/float_place.h), centered on it.
            readonly property Item bar: (win.runToolbarItem as RunToolbar)?.surface ?? null
            readonly property rect barRect: bar ? Qt.rect(bar.x, bar.y, bar.width, bar.height) : Qt.rect(0, 0, 0, 0)
            readonly property size area: Qt.size(fugueCanvas.width, fugueCanvas.height)
            maxHeight: Math.max(12 * look.unit, FloatPlace.roomOnSide(barRect, wantedHeight, area, 2 * look.unit))
            x: FloatPlace.panelPlace(barRect, Qt.size(width, height), area, 2 * look.unit, wantedHeight).x
            y: FloatPlace.panelPlace(barRect, Qt.size(width, height), area, 2 * look.unit, wantedHeight).y
            onCancelRequested: run => App.runQueue.cancel(run)
            onFrontRequested: run => App.runQueue.moveToFront(run)
            onFolderRequested: run => App.runQueue.openFolder(run)
            onCloseRequested: App.runBar.queueOpen = false
            thumbOf: path => App.mediaLibrary.thumbSource(path, 64)
            onResultsRequested: run => win.showRunResults(run)
            // Escape closes it while it has the focus, so the run's cancel keys sleep then.
            Binding {
                target: App.runs
                property: "yieldKeys"
                value: runQueuePanel.activeFocus
            }
        }
    }

    // The side panels (#56): the node library (N) and the workflows panel (W), one open at a time
    // on the left or the right of the canvas, which gives up their width. Made when one is
    // open and dropped when none is, and not at all in a window without them: the window keeps
    // few items (#5). The panels know nothing of the canvas or of App; what they ask is done here.
    property Item sidePanelItem: null
    // A run whose results the workflows panel opens on once it is made (see showRunResults).
    property string pendingResultsRun: ""
    // What a finished row of the run queue asks for (#133): the media browser opens on that run's
    // results, in the workflows panel; the nodes of the saved workflow it came from show them too
    // when that workflow is the one open. A window without side panels only has the canvas, and
    // says when that cannot show them.
    function showRunResults(run) {
        const onCanvas = App.runs.showResults(run, App.runQueue.workflowOf(run))
        if (!sidePanelsWanted || App.mediaLibrary === null) {
            if (!onCanvas)
                App.workflows.notify(qsTr("Open the workflow that run started from to see its results."))
            return
        }
        const panel = App.sidePanels.tab === "workflows" ? sidePanelContent as WorkflowsPanel : null
        if (panel) {
            panel.showRun(run)
        } else {
            pendingResultsRun = run
            App.sidePanels.tab = "workflows"
        }
    }
    // The tab strip (#159) on the left edge under the bars: made while the registry shows it
    // (focus mode hides it), and not at all in a window without side panels.
    // The App view has its own rail, so the strip and its panels are not shown there (they come back in the graph).
    readonly property bool sidePanelsVisible: sidePanelsWanted && !appShown
    onSidePanelsVisibleChanged: syncSidePanel()
    readonly property bool stripWanted: sidePanelsVisible && win.chromeIsShown("side-tabs")
    readonly property real stripWidth: stripWanted && stripLoader.item ? (stripLoader.item as Item).width : 0
    // The most of the window the panel may take: the canvas keeps at least 160px.
    readonly property real sidePanelWidth: Math.max(0, Math.min(App.sidePanels ? App.sidePanels.width : 0, windowRoot.width - stripWidth - 160))
    property Item sidePanelContent: null
    property string sidePanelShown: ""
    readonly property bool sidePanelsWanted: App.sidePanels !== null && App.nodeLibrary !== null && App.recentFiles !== null
    // The width the canvas gives up on `side` ("left" or "right"). The layout mirrors with the
    // window, and so does its margin, so a margin is asked for on the side it will be drawn on.
    function sidePanelMargin(side) {
        if (!sidePanelItem || App.sidePanels.dock !== side)
            return 0
        return sidePanelWidth
    }
    function syncSidePanel() {
        const tab = sidePanelsVisible ? App.sidePanels.tab : ""
        if (tab === "") {
            // Closing the panel from inside it (Escape) gives the keys back to the canvas, which opened it.
            let hadFocus = false
            for (let at = win.activeFocusItem; sidePanelItem && at && !hadFocus; at = at.parent)
                hadFocus = at === sidePanelItem
            if (sidePanelItem)
                sidePanelItem.destroy()
            if (hadFocus && fugueCanvas.visible)
                fugueCanvas.forceActiveFocus()
            sidePanelItem = null
            sidePanelContent = null
            sidePanelShown = ""
            return
        }
        if (!sidePanelItem)
            sidePanelItem = sidePanelComponent.createObject(windowRoot) as Item
        if (sidePanelContent && sidePanelShown !== tab) {
            sidePanelContent.destroy()
            sidePanelContent = null
        }
        if (!sidePanelContent) {
            const area = (sidePanelItem as SidePanel).contentArea
            sidePanelContent = (tab === "nodes" ? nodeLibraryComponent : workflowsPanelComponent).createObject(area) as Item
            ;(sidePanelItem as SidePanel).focusTarget = tab === "nodes" ? (sidePanelContent as NodeLibraryPanel).focusTarget
                                                                         : (sidePanelContent as WorkflowsPanel).focusTarget
        }
        sidePanelShown = tab
    }
    Loader {
        id: stripLoader
        parent: windowRoot
        active: win.stripWanted
        y: mainLayout.y + win.cardTop
        height: win.cardBottom - win.cardTop
        sourceComponent: SideTabStrip {
            look: App.theme.look
            panels: App.sidePanels.panels
            onTabRequested: tab => App.sidePanels.toggle(tab)
            onActionRequested: command => App.commands.trigger(command)
        }
        onLoaded: (item as Item).height = Qt.binding(() => stripLoader.height)
    }
    Connections {
        target: App.sidePanels
        function onTabChanged() { win.syncSidePanel() }
        function onFocusRequested() {
            win.syncSidePanel()
            ;(win.sidePanelItem as SidePanel)?.focusFirst()
        }
    }
    Component {
        id: sidePanelComponent
        SidePanel {
                look: App.theme.look
            title: App.sidePanels.tab === "nodes" ? qsTr("Node library") : qsTr("Workflows")
            // The window's tone; the results browser is ember (docs/canvas-look.md 20.1, 4).
            tone: App.sidePanels.tab === "workflows" && (win.sidePanelContent as WorkflowsPanel)?.view === "results" ? "ember" : "dusk"
            dock: App.sidePanels.dock
            width: win.sidePanelWidth
            minWidth: App.sidePanels.minWidth
            maxWidth: App.sidePanels.maxWidth
            x: dock === "right" ? windowRoot.width - width : win.stripWidth
            y: mainLayout.y + win.cardTop
            height: win.cardBottom - win.cardTop
            gap: win.cardHalf
            onCloseRequested: App.sidePanels.tab = ""
            onDockRequested: dock => App.sidePanels.dock = dock
            onWidthRequested: width => App.sidePanels.width = width
        }
    }
    Component {
        id: nodeLibraryComponent
        NodeLibraryPanel {
            anchors.fill: parent
            look: App.theme.look
            rows: App.nodeLibrary.rows
            query: App.nodeLibrary.query
            nodeCount: App.nodeLibrary.nodeCount
            onQueryEdited: query => App.nodeLibrary.query = query
            onCategoryToggled: key => App.nodeLibrary.toggle(key)
            onCloseRequested: App.sidePanels.tab = ""
            // A group of the library deleted after its question (#156): the one call Settings >
            // Packs > Library uses; the registry is read again, so the row and the search result
            // go at once. The window's notice line says it, or why not.
            onGroupDeleted: (typeId, name) => {
                const packs = App.settings ? App.settings.packs : null
                const done = packs !== null && packs.removeLibraryGroup(typeId)
                App.canvasModel.groups.announceLibraryResult(qsTr("Deleted %1 from the library").arg(name),
                    done ? "" : (packs && packs.failure !== "" ? packs.failure
                                 : qsTr("%1 could not be deleted from the library.").arg(name)))
            }
            // A click adds the node in the middle of the view; a drop, where it was let go.
            onNodeActivated: typeId => {
                const p = fugueCanvas.toWorld(fugueCanvas.width / 2, fugueCanvas.height / 2)
                App.canvasModel.addNode(typeId, p.x, p.y)
            }
            onNodeDropped: (typeId, sceneX, sceneY) => {
                const at = fugueCanvas.mapFromItem(null, sceneX, sceneY)
                if (at.x < 0 || at.y < 0 || at.x > fugueCanvas.width || at.y > fugueCanvas.height)
                    return
                const p = fugueCanvas.toWorld(at.x, at.y)
                App.canvasModel.addNode(typeId, p.x, p.y)
            }
        }
    }
    Component {
        id: workflowsPanelComponent
        WorkflowsPanel {
            id: workflowsPanel
            anchors.fill: parent
            look: App.theme.look
            tabs: App.workflows.tabs
            recent: App.recentFiles.entries
            onTabRequested: index => App.workflows.current = index
            onTabCloseRequested: index => App.workflows.closeTab(index)
            onFileRequested: path => {
                if (!App.workflows.openPath(path))
                    App.recentFiles.forget(path)
            }
            onCloseRequested: App.sidePanels.tab = ""
            // What the browser remembers: the view, the mode and the favorites filter (media_state.h).
            view: App.mediaLibrary.resultsView
            resultsMode: App.mediaLibrary.resultsMode
            onlyFavorites: App.mediaLibrary.onlyFavorites
            onViewChanged: App.mediaLibrary.resultsView = view
            onResultsModeChanged: App.mediaLibrary.resultsMode = resultsMode
            onFavoritesToggled: App.mediaLibrary.onlyFavorites = !App.mediaLibrary.onlyFavorites
            // Made for a run's results (the queue's row asked): it opens on that run.
            Component.onCompleted: {
                if (win.pendingResultsRun !== "") {
                    showRun(win.pendingResultsRun)
                    win.pendingResultsRun = ""
                }
            }
            // The results of the current workflow (#133): looked up when the view opens, when the
            // workflow changes, and when a run ends; the list is replaced only when it differs.
            function refreshResults() {
                const rows = workflowsPanel.resultsRun !== "" ? App.mediaLibrary.resultsOfRun(workflowsPanel.resultsRun)
                                                              : App.mediaLibrary.resultsOf(workflowsPanel.workflowPath)
                const key = list => JSON.stringify(list.map(r => [r.path, r.favorite]))
                if (key(rows) !== key(workflowsPanel.results))
                    workflowsPanel.results = rows
            }
            onResultsRequested: refreshResults()
            onResultChosen: item => {
                workflowsPanel.resultDetails = App.mediaLibrary.detailsOf(item.path, item.run, item.node)
                workflowsPanel.resultCaps = App.mediaLibrary.capabilitiesOf(item.path)
            }
            onResultAction: (action, item) => {
                if (action === "open") App.mediaLibrary.openFile(item.path)
                else if (action === "folder") App.mediaLibrary.openFolder(item.path)
                else if (action === "copy") App.mediaLibrary.copyPath(item.path)
                else if (action === "favorite") App.mediaLibrary.toggleFavorite(item.path)
                else if (action === "send") App.workflows.sendImage(item.path)
                else if (action === "workflow") App.workflows.openPath(item.path)
                else if (action === "rerun") {
                    if (App.workflows.openPath(item.path))
                        App.commands.trigger("run.start")
                }
                else if (action === "delete") App.mediaLibrary.trash(item.path)
            }
            Connections {
                target: App.runQueue
                function onChanged() { if (workflowsPanel.view === "results") Qt.callLater(workflowsPanel.refreshResults) }
            }
            Connections {
                target: App.mediaLibrary
                function onResultsChanged() { if (workflowsPanel.view === "results") Qt.callLater(workflowsPanel.refreshResults) }
            }
        }
    }

    NodeSearchPopup {
        id: searchPopup
        canvas: fugueCanvas
    }
}
