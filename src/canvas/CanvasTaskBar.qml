pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import Fugue
import "widgets"

// The Canvas view's task bar (#231, docs/canvas-actions-design.md 4; Photoshop's Contextual Task Bar is the reference,
// docs/ui-ergonomics.md References): edit the canvas from the view without opening a workflow. It lies over the picture
// (the host makes it the picture's size) and floats under the selection, 2u clear of it, flipping above it when there is
// no room and staying 4u inside the picture; it follows a pan or a zoom. It shows while there is a selection (the
// rectangle on the picture, or a Quick Mask selection while Quick Mask is on), and while its runs go or its last word
// stands. Simple state, from the start: the action menu (Auto, then the actions; in a narrow area its glyph alone), the
// prompt (Enter generates; Up and Down in an empty field step through the earlier prompts; the clock opens the last 20),
// Generate (the one primary; Cancel while this bar's runs wait or go, Escape too), Advanced (the popover of
// CanvasTaskAdvanced.qml: the action's other inputs, Count, Edit workflow) and ⋯ (Clear prompt history…). Under
// them, only when there is something to say: the progress ("Generating… 45%"), what is missing and its Download, the
// latest version to go back to, or the last error in plain words. While a run goes the selection's outline shimmers
// (marching dashes in the accent). Everything it does is App.canvasActions (canvas/canvas_actions.h).
Item {
    id: bar
    objectName: "canvasTaskBar"
    required property CanvasLook look
    // The view's context: the canvas, the revision shown and whether it is the latest (the Head).
    property string canvasId: ""
    property string revision: ""
    property bool onHead: false
    // The layer Generate reads instead of the whole picture ("" for the picture as it looks), set in the Layers panel.
    property string regionLayer: ""
    // The rectangle on the picture (canvas pixels; width 0: none), and the Quick Mask selection while Quick Mask is on.
    property var rect: ({x: 0, y: 0, width: 0, height: 0})
    property MaskLayer quickMask: null
    // How canvas pixels map onto this item: the picture's origin and scale (the viewer's).
    property real originX: 0
    property real originY: 0
    property real zoom: 1
    signal backToLatest()
    // Edit workflow opened the action's graph as a tab: the host shows the Graph (an area makes it active, the dialog closes).
    signal workflowOpened()

    readonly property real u: look.unit
    readonly property CanvasActions actions: App.canvasActions
    // The Quick Mask selection's bounds, read again after each stroke (not each dab).
    property var maskBounds: ({})
    function readMaskBounds() { maskBounds = actions && quickMask && !quickMask.empty ? actions.selectionBounds(quickMask) : ({}) }
    Connections {
        target: bar.quickMask
        function onHistoryChanged() { bar.readMaskBounds() }
    }
    onQuickMaskChanged: readMaskBounds()
    readonly property bool usesMask: quickMask !== null && maskBounds.width !== undefined
    // The selection in canvas pixels: the Quick Mask's bounds, else the rectangle; null for none.
    readonly property var selection: usesMask ? maskBounds : rect && rect.width > 0 && rect.height > 0 ? rect : null
    readonly property rect selectionBox: selection ? Qt.rect(originX + selection.x * zoom, originY + selection.y * zoom,
                                                            selection.width * zoom, selection.height * zoom) : Qt.rect(0, 0, 0, 0)

    // This canvas's runs, and what the bar says of them.
    readonly property bool mine: actions !== null && actions.canvas === canvasId
    readonly property bool busy: actions !== null && actions.busy && mine
    readonly property bool otherBusy: actions !== null && actions.busy && !mine
    property string chosen: "auto"
    readonly property var actionRows: actions ? actions.actions : []
    // The action Generate runs: the chosen one, or the one Auto stands for (Generative fill until there are others).
    readonly property var current: {
        const id = chosen === "auto" ? "generative-fill" : chosen
        return actionRows.find(row => row.id === id) ?? ({})
    }
    readonly property bool missingModels: (current.missing ?? 0) > 0
    // An action on the canvas bounds (Expand) grows the canvas by a side and an amount from the bar, not by a selection
    // (design 4): the bar stays up while it is the chosen action, so it can be used with nothing selected.
    readonly property bool boundsAction: current.selection === "bounds"
    property string expandSide: "right"
    property int expandPercent: 25
    readonly property string expandText: expandSide === "left" ? qsTr("Left, %1%").arg(expandPercent)
                                       : expandSide === "top" ? qsTr("Top, %1%").arg(expandPercent)
                                       : expandSide === "bottom" ? qsTr("Bottom, %1%").arg(expandPercent)
                                       : qsTr("Right, %1%").arg(expandPercent)
    readonly property bool hasPrompt: promptField.text.trim() !== ""
    readonly property string blocker: !onHead ? qsTr("Generate works on the latest version")
                                    : otherBusy ? qsTr("Another canvas is generating")
                                    : current.broken === true ? current.reason
                                    : missingModels ? qsTr("Download the models first")
                                    : selection === null && !boundsAction ? qsTr("Select an area first")
                                    : current.needsPrompt === true && !hasPrompt ? qsTr("Type what to put in the selection")
                                    : ""
    readonly property bool hasWord: mine && actions !== null && (actions.error !== "" || actions.notice !== "")
    readonly property bool shown: actions !== null && canvasId !== "" && (selection !== null || busy || hasWord || boundsAction)
    visible: shown
    // Where the bar may go when the picture is too narrow for it (a small area): the host's whole width, in this item's
    // coordinates. Then it docks across the foot of the picture rather than squeezing its controls.
    property real roomX: 0
    property real roomWidth: width
    readonly property real minWidth: 72 * u
    readonly property bool docked: width - 8 * u < minWidth
    // Narrow: the action menu folds to its glyph (design 4: below 360 px).
    readonly property bool narrow: box.width < 90 * u

    function generate() {
        if (!actions || busy)
            return
        if (blocker !== "") {
            actions.clearError()
            return
        }
        const request = {action: chosen, canvas: canvasId, revision: revision, prompt: promptField.text}
        if (regionLayer !== "")
            request.layer = regionLayer
        if (boundsAction) {
            request.expand = {side: expandSide, percent: expandPercent}
        } else if (usesMask) {
            const stored = actions.storeSelection(quickMask)
            if (!stored.ok)
                return
            request.selection = stored.hash
            request.rect = {x: stored.x, y: stored.y, width: stored.width, height: stored.height}
        } else {
            request.rect = selection
        }
        historyStep = -1
        actions.generate(request)
    }
    // Escape while this bar's runs go ends them (the view asks; the prompt field too). True when it did.
    function cancelIfBusy() {
        if (!busy)
            return false
        actions.cancel()
        return true
    }

    // Prompt history (design 4): Up and Down step through it while the field is empty or shows an earlier prompt.
    property int historyStep: -1
    readonly property var history: actions && historyTick >= 0 ? actions.history(current.id ?? "") : []
    property int historyTick: 0
    Connections {
        target: bar.actions
        function onHistoryChanged() { bar.historyTick++ }
    }
    function stepHistory(older) {
        const list = history
        if (list.length === 0 || (promptField.text !== "" && (historyStep < 0 || promptField.text !== list[historyStep])))
            return false
        historyStep = Math.max(-1, Math.min(list.length - 1, historyStep + (older ? 1 : -1)))
        promptField.text = historyStep < 0 ? "" : list[historyStep]
        return true
    }

    // The menus under the button that asked.
    Component {
        id: menuComponent
        WidgetMenu {
            id: menu
            look: bar.look
            parent: Overlay.overlay
            onClosed: menu.destroy()
        }
    }
    function openMenu(button: Item, entries, picked, wide: real, name: string) {
        const at = button.mapToItem(Overlay.overlay, 0, button.height)
        const menu = menuComponent.createObject(bar, {wantX: at.x, wantY: at.y, width: Math.max(button.width, wide * u), anchorHeight: button.height,
                                                      entries: entries, searchable: entries.length > 12}) as WidgetMenu
        menu.objectName = name
        menu.picked.connect(picked)
        menu.open()
    }
    function chooseAction() {
        actions.refresh() // an action added to the library, or saved, since the bar was drawn
        const rows = [{text: qsTr("Auto"), value: "auto", checked: chosen === "auto", note: qsTr("Picks the action from the selection and the prompt")}]
        for (const row of actionRows)
            rows.push({text: row.name, value: row.id, checked: chosen === row.id, enabled: !row.broken,
                       note: row.broken ? row.reason : row.description})
        openMenu(actionButton, rows, value => { bar.chosen = value; bar.historyStep = -1 }, 64, "canvasTaskActionMenu")
    }
    function chooseExpand() {
        const rows = []
        for (const side of ["left", "right", "top", "bottom"])
            for (const percent of [25, 50]) {
                const name = side === "left" ? qsTr("Left, %1%") : side === "right" ? qsTr("Right, %1%") : side === "top" ? qsTr("Top, %1%") : qsTr("Bottom, %1%")
                rows.push({text: name.arg(percent), value: side + ":" + percent, checked: expandSide === side && expandPercent === percent})
            }
        openMenu(expandButton, rows, value => { const parts = value.split(":"); bar.expandSide = parts[0]; bar.expandPercent = Number(parts[1]) }, 40, "canvasTaskExpandMenu")
    }
    function chooseHistory() {
        openMenu(historyButton, history.slice(0, 20).map(text => ({text: text, value: text})),
                 value => { promptField.text = value; promptField.forceActiveFocus() }, 72, "canvasTaskHistoryMenu")
    }
    // Edit workflow (design 7): the chosen action's graph, in a tab of the Graph. The error line says why it did not open.
    function editWorkflow() {
        if (actions && actions.editWorkflow(current.id ?? chosen).ok)
            workflowOpened()
    }
    function more() {
        openMenu(moreButton, [{text: qsTr("Clear prompt history…"), value: "clear", enabled: history.length > 0}],
                 value => { if (value === "clear") clearAsk.open() }, 48, "canvasTaskMoreMenu")
    }
    function advanced() {
        if (advancedPopover.opened)
            advancedPopover.close()
        else
            advancedPopover.openFor(advancedButton)
    }

    // The shimmer on the selection while it is redrawn: the accent's dashes marching on the dark halo of the frame.
    Shape {
        id: shimmer
        objectName: "canvasTaskShimmer"
        anchors.fill: parent
        visible: bar.busy && bar.selection !== null
        opacity: bar.actions !== null && bar.actions.liveFrame !== "" ? 0.5 : 1 // dimmed while a frame shows in the selection
        readonly property rect b: bar.selectionBox
        property real offset: 0
        NumberAnimation on offset {
            running: shimmer.visible
            from: 0
            to: 14
            duration: 700
            loops: Animation.Infinite
        }
        ShapePath {
            strokeColor: Qt.rgba(0, 0, 0, 0.6)
            strokeWidth: 3
            fillColor: "transparent"
            startX: shimmer.b.left; startY: shimmer.b.top
            PathLine { x: shimmer.b.right; y: shimmer.b.top }
            PathLine { x: shimmer.b.right; y: shimmer.b.bottom }
            PathLine { x: shimmer.b.left; y: shimmer.b.bottom }
            PathLine { x: shimmer.b.left; y: shimmer.b.top }
        }
        ShapePath {
            strokeColor: bar.look.accent
            strokeWidth: 2
            strokeStyle: ShapePath.DashLine
            dashPattern: [4, 3]
            dashOffset: shimmer.offset
            fillColor: "transparent"
            startX: shimmer.b.left; startY: shimmer.b.top
            PathLine { x: shimmer.b.right; y: shimmer.b.top }
            PathLine { x: shimmer.b.right; y: shimmer.b.bottom }
            PathLine { x: shimmer.b.left; y: shimmer.b.bottom }
            PathLine { x: shimmer.b.left; y: shimmer.b.top }
        }
    }

    Shadow {
        target: box
        level: 2
        look: bar.look
    }
    Rectangle {
        id: box
        objectName: "canvasTaskBox"
        readonly property real inset: 2 * bar.u
        width: bar.docked ? Math.max(0, Math.min(bar.roomWidth - 8 * bar.u, 150 * bar.u)) : Math.min(bar.width - 8 * bar.u, 150 * bar.u)
        height: column.implicitHeight + 2 * inset
        // Under the selection, 2u clear; above it when there is no room; at the foot when neither fits (design 4).
        readonly property real below: bar.selectionBox.y + bar.selectionBox.height + 2 * bar.u
        readonly property real above: bar.selectionBox.y - 2 * bar.u - height
        readonly property real foot: bar.height - 4 * bar.u - height
        x: bar.docked ? bar.roomX + (bar.roomWidth - width) / 2
         : bar.selection === null ? (bar.width - width) / 2
         : Math.max(4 * bar.u, Math.min(bar.width - 4 * bar.u - width, bar.selectionBox.x + (bar.selectionBox.width - width) / 2))
        y: bar.docked || bar.selection === null ? foot : below <= foot ? below : above >= 4 * bar.u ? above : foot
        radius: bar.look.popupCorner
        color: bar.look.popup
        border.width: 1
        border.color: bar.look.hairline
        // The bar takes the pointer over itself only (a press here never draws a rectangle under it); the picture keeps it
        // everywhere else.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
        }
        LayoutMirroring.enabled: Application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            id: column
            x: box.inset
            y: box.inset
            width: box.width - 2 * box.inset
            spacing: bar.u
            RowLayout {
                Layout.fillWidth: true
                spacing: bar.u
                ViewButton {
                    id: actionButton
                    objectName: "canvasTaskAction"
                    look: bar.look
                    glyph: bar.chosen === "auto" ? "wand-sparkles" : (bar.current.glyph ?? "sparkles")
                    text: bar.narrow ? "" : bar.chosen === "auto" ? qsTr("Auto") : (bar.current.name ?? "")
                    trailingGlyph: "chevron-down"
                    tip: bar.chosen === "auto" ? qsTr("Auto: picks the action from the selection and the prompt") : (bar.current.description ?? "")
                    onTapped: bar.chooseAction()
                }
                ThemedTextField {
                    id: promptField
                    objectName: "canvasTaskPrompt"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 7 * bar.u
                    Layout.minimumWidth: 24 * bar.u
                    padding: bar.u
                    leftPadding: 2 * bar.u
                    rightPadding: historyButton.width + bar.u
                    font.pixelSize: bar.look.labelSize
                    color: bar.look.text
                    // The hint is drawn here, not as Material's placeholder, which floats it over the field once focused.
                    Text {
                        objectName: "canvasTaskPromptHint"
                        x: promptField.leftPadding
                        width: promptField.width - promptField.leftPadding - promptField.rightPadding
                        anchors.verticalCenter: parent.verticalCenter
                        visible: promptField.text === "" && promptField.preeditText === ""
                        elide: Text.ElideRight
                        color: bar.look.label
                        font.pixelSize: bar.look.labelSize
                        text: bar.boundsAction ? qsTr("Describe the scene, or leave empty")
                            : bar.current.hasPrompt === false ? qsTr("No prompt needed: press Generate")
                            : bar.selection === null ? qsTr("Select an area first") : qsTr("Describe what to put in the selection")
                    }
                    maximumLength: 2000
                    Accessible.name: qsTr("Prompt")
                    background: Rectangle {
                        radius: bar.look.smallCorner
                        color: bar.look.well
                        border.width: promptField.activeFocus ? 2 : 1
                        border.color: promptField.activeFocus ? bar.look.accent : bar.look.hairline
                    }
                    onTextEdited: if (bar.actions && bar.mine && bar.actions.error !== "") bar.actions.clearError()
                    // Enter generates; Ctrl+Enter is the Graph's Run (run.start) and passes on.
                    Keys.onReturnPressed: event => {
                        if (event.modifiers & (Qt.ControlModifier | Qt.ShiftModifier | Qt.AltModifier)) {
                            event.accepted = false
                            return
                        }
                        bar.generate()
                    }
                    Keys.onEnterPressed: event => {
                        if (event.modifiers & (Qt.ControlModifier | Qt.ShiftModifier | Qt.AltModifier)) {
                            event.accepted = false
                            return
                        }
                        bar.generate()
                    }
                    Keys.onUpPressed: event => { event.accepted = bar.stepHistory(true) }
                    Keys.onDownPressed: event => { event.accepted = bar.stepHistory(false) }
                    Keys.onEscapePressed: event => { event.accepted = bar.cancelIfBusy() }
                    ViewButton {
                        id: historyButton
                        objectName: "canvasTaskHistory"
                        anchors.right: parent.right
                        anchors.rightMargin: 2
                        anchors.verticalCenter: parent.verticalCenter
                        look: bar.look
                        glyph: "history"
                        flat: true
                        compact: true
                        enabled: bar.history.length > 0
                        tip: bar.history.length > 0 ? qsTr("Earlier prompts (Up and Down step through them)") : qsTr("No earlier prompts yet")
                        onTapped: bar.chooseHistory()
                    }
                }
                ViewButton {
                    id: generateButton
                    objectName: "canvasTaskGenerate"
                    look: bar.look
                    visible: !bar.busy
                    primary: true
                    enabled: bar.blocker === ""
                    text: qsTr("Generate")
                    tip: bar.blocker !== "" ? bar.blocker : qsTr("Generate into the selection (Enter)")
                    onTapped: bar.generate()
                }
                ViewButton {
                    objectName: "canvasTaskCancel"
                    look: bar.look
                    visible: bar.busy
                    text: qsTr("Cancel")
                    tip: qsTr("Stop generating; nothing is added (Esc)")
                    onTapped: bar.actions.cancel()
                }
                ViewButton {
                    id: advancedButton
                    objectName: "canvasTaskAdvancedButton"
                    look: bar.look
                    glyph: bar.narrow ? "settings" : ""
                    text: bar.narrow ? "" : qsTr("Advanced")
                    flat: true
                    current: advancedPopover.opened
                    accented: false
                    tip: qsTr("Advanced: the action's other settings, Count and Edit workflow")
                    onTapped: bar.advanced()
                }
                ViewButton {
                    id: moreButton
                    objectName: "canvasTaskMore"
                    look: bar.look
                    glyph: "ellipsis"
                    flat: true
                    tip: qsTr("More")
                    onTapped: bar.more()
                }
            }
            // Expand: which side the canvas grows on and by how much of the picture's side (one side per run).
            RowLayout {
                id: expandRow
                objectName: "canvasTaskExpandRow"
                Layout.fillWidth: true
                visible: bar.boundsAction
                spacing: bar.u
                Text {
                    color: bar.look.label
                    font.pixelSize: bar.look.labelSize
                    text: qsTr("Grow the canvas")
                }
                ViewButton {
                    id: expandButton
                    objectName: "canvasTaskExpand"
                    look: bar.look
                    text: bar.expandText
                    trailingGlyph: "chevron-down"
                    tip: qsTr("The side to grow, and how much of the picture's size")
                    onTapped: bar.chooseExpand()
                }
            }
            // What there is to say, one line, with its one action: progress, the missing models, the latest version, or the
            // last error or notice. Nothing at all when there is nothing to say.
            RowLayout {
                id: statusRow
                objectName: "canvasTaskStatusRow"
                Layout.fillWidth: true
                spacing: 2 * bar.u
                readonly property string of: bar.busy && bar.actions.runTotal > 1 ? qsTr("%1 of %2").arg(bar.actions.runNumber).arg(bar.actions.runTotal) : ""
                readonly property string word: bar.busy ? (bar.actions.running ? (bar.actions.progress >= 0 ? (of !== "" ? qsTr("Generating %1… %2%").arg(of).arg(Math.round(bar.actions.progress * 100))
                                                                                                                       : qsTr("Generating… %1%").arg(Math.round(bar.actions.progress * 100)))
                                                                                                             : of !== "" ? qsTr("Generating %1…").arg(of) : qsTr("Generating…"))
                                                                                : qsTr("Waiting for the GPU…"))
                                              : bar.actions && bar.actions.downloading ? qsTr("Downloading %1").arg(bar.actions.downloadText)
                                              : bar.hasWord && bar.actions.error !== "" ? bar.actions.error
                                              : bar.hasWord ? bar.actions.notice
                                              : !bar.onHead ? qsTr("Generate works on the latest version.")
                                              : bar.missingModels ? qsTr("%1 needs %n models, %2.", "", bar.current.missing).arg(bar.current.name).arg(bar.current.size)
                                              : ""
                readonly property bool isError: !bar.busy && bar.hasWord && bar.actions.error !== ""
                visible: word !== ""
                Text {
                    objectName: "canvasTaskStatus"
                    Layout.fillWidth: true
                    Layout.minimumHeight: 6 * bar.u
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.Wrap
                    color: statusRow.isError ? bar.look.error : bar.look.label
                    font.pixelSize: bar.look.labelSize
                    font.features: { "tnum": 1 }
                    text: statusRow.word
                }
                ViewButton {
                    objectName: "canvasTaskDownload"
                    look: bar.look
                    compact: true
                    visible: !bar.busy && !(bar.actions && bar.actions.downloading) && bar.missingModels && (bar.current.download ?? "") !== ""
                             && (bar.onHead || bar.hasWord)
                    glyph: "download"
                    text: qsTr("Download")
                    tip: bar.current.download ?? ""
                    onTapped: bar.actions.download(bar.current.id)
                }
                ViewButton {
                    objectName: "canvasTaskBackToLatest"
                    look: bar.look
                    compact: true
                    visible: !bar.busy && !bar.hasWord && !bar.onHead
                    text: qsTr("Back to latest")
                    tip: qsTr("Show the latest version of the canvas")
                    onTapped: bar.backToLatest()
                }
            }
        }
    }

    CanvasTaskAdvanced {
        id: advancedPopover
        look: bar.look
        actions: bar.actions
        current: bar.current
        onEditWorkflow: bar.editWorkflow()
    }

    // Clear prompt history…: asks first, Keep highlighted (the safe answer, D8).
    Question {
        id: clearAsk
        objectName: "canvasTaskClearAsk"
        onOpened: keepButton.forceActiveFocus()
        ColumnLayout {
            anchors.fill: parent
            spacing: 4 * bar.u
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Clear the prompt history of this project? The prompts you typed in the task bar are forgotten.")
            }
            // Keep (the safe answer, highlighted and focused), then Clear in the danger tint after the wider gap.
            QuestionAnswers {
                spacing: 4 * bar.u
                ThemedButton {
                    id: keepButton
                    objectName: "canvasTaskClearKeep"
                    text: qsTr("Keep")
                    highlighted: true
                    onClicked: clearAsk.close()
                }
                ThemedButton {
                    objectName: "canvasTaskClearYes"
                    text: qsTr("Clear")
                    danger: true
                    onClicked: { clearAsk.close(); bar.actions.clearHistory() }
                }
            }
        }
    }
}
