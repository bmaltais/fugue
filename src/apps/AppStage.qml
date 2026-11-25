pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../kit"
import "../canvas/widgets"
import "../menu"

// The stage of the App view, in the layout of ComfyUI App Mode (layout and behaviour only): the result large on the start side
// of the page, a thin progress line along the result's top edge, a toolbar over it, the size and zoom under it, and under that the takes (the
// runs of this session as thumbnails). AppView holds all the logic; this draws it.
//
//   toolbar  while a run goes: Cancel (a normal button: danger red is for deletion) and where the run is. Otherwise Run again,
//            Reuse inputs (this take's inputs back in the form), Save as…, Clear (the stage and the takes; the files stay), and "…" (Open large, Show in folder, Use as
//            input, Compare with the input)
//   result   a picture in CompareView (single mode, as the large view's): the wheel zooms about the pointer, a drag pans once
//            zoomed, a double click fits (at Fit it opens the large view), Z toggles 100%, C compares with the input picture,
//            Home or 0 fits. A video or sound plays, a text is text. While a run goes the Sampler's step picture shows when
//            there is one.
//   under    "1392 × 752 · 100%" quietly, with Fit and 100% at the end
//   takes    AppTakes, then the shown take's inputs in one quiet line
Item {
    id: stage
    objectName: "appStage"
    required property AppView appView
    required property CanvasLook look
    readonly property real u: look.unit
    readonly property var results: appView.shownResults
    property int resultIndex: 0
    readonly property var current: results.length > 0 ? results[Math.min(resultIndex, results.length - 1)] : null
    readonly property bool isImage: current !== null && current.kind === "image"
    property bool comparing: false
    // What the copy button puts on the clipboard: the message, then one line per detail (its hint after it); the detail that repeats the message adds only its hint to the first line.
    readonly property string errorReport: {
        const lines = [stage.appView.status === "failed" ? stage.appView.error : stage.notice]
        if (stage.appView.status === "failed") {
            for (const d of stage.appView.details) {
                if (d.message === lines[0]) // the headline again: only its hint is new
                    lines[0] = d.hint !== "" ? lines[0] + " " + d.hint : lines[0]
                else
                    lines.push(d.hint !== "" ? d.message + " " + d.hint : d.message)
            }
        }
        return lines.join("\n")
    }
    property var inputPicture: ({})
    readonly property bool canCompare: isImage && inputPicture.url !== undefined
    readonly property bool hasInputImage: appView.imageKey() !== ""
    property string notice
    property bool detailsOpen: false
    readonly property alias viewer: view

    // B of the Compare: the picture the shown take ran with, else (an older take, a file gone) the form's own.
    function refreshInput() {
        const taken = appView.takeInputPicture()
        inputPicture = taken.url !== undefined ? taken : appView.maskPicture()
    }
    Component.onCompleted: refreshInput()
    Connections {
        target: stage.appView
        function onValueChanged() { stage.refreshInput() }
        function onAppChanged() { stage.refreshInput(); stage.comparing = false; stage.detailsOpen = false; stage.notice = "" }
        function onRunChanged() { stage.refreshInput(); if (stage.appView.busy) stage.notice = ""; if (stage.resultIndex >= stage.results.length) stage.resultIndex = 0 }
    }
    onCurrentChanged: view.fit()
    function toggleCompare() {
        if (comparing) {
            comparing = false
        } else if (canCompare) {
            refreshInput()
            comparing = true
        }
    }
    function toggleActual() { if (view.actual) view.fit(); else view.actualSize() }
    function openLarge() { if (current !== null) largeView.openFor(current) }

    // Keys of the stage (the large view's keymap, docs/ui-ergonomics.md D5): they reach it from the picture once it has the focus.
    Keys.onPressed: event => {
        if (!stage.isImage || (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)))
            return
        const middle = Qt.point(view.width / 2, view.height / 2)
        if (event.key === Qt.Key_Z) stage.toggleActual()
        else if (event.key === Qt.Key_C) stage.toggleCompare()
        else if (event.key === Qt.Key_Home || event.key === Qt.Key_0) view.fit()
        else if (event.key === Qt.Key_1) view.actualSize()
        else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) view.zoomAt(middle.x, middle.y, 1.25)
        else if (event.key === Qt.Key_Minus) view.zoomAt(middle.x, middle.y, 0.8)
        else return
        event.accepted = true
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: stage.u

        RowLayout {
            id: toolbar
            objectName: "appToolbar"
            Layout.fillWidth: true
            Layout.maximumHeight: 7 * stage.u // a layout with nothing visible would otherwise take the room
            visible: stage.appView.busy || stage.current !== null || stage.appView.canClear
            spacing: stage.u
            KitChip {
                objectName: "appCancel"
                look: stage.look
                pill: true
                text: qsTr("Cancel this run")
                glyph: "x"
                bordered: true
                visible: stage.appView.busy
                onTapped: stage.appView.cancel()
            }
            Text {
                objectName: "appProgressText"
                Layout.fillWidth: true
                visible: stage.appView.busy
                text: stage.appView.status === "waiting" ? qsTr("Waiting for its turn…")
                    : stage.appView.progress >= 0 ? qsTr("Working… %1%").arg(Math.round(stage.appView.progress * 100))
                    : qsTr("Working…")
                color: stage.look.label
                font.pixelSize: stage.look.labelSize
                elide: Text.ElideRight
            }
            KitChip {
                objectName: "appRunAgain"
                look: stage.look
                pill: true
                text: qsTr("Run again")
                glyph: "refresh-cw"
                visible: !stage.appView.busy && stage.appView.takeIndex >= 0
                enabled: stage.appView.canRun
                tip: qsTr("Run again with this take's inputs")
                onTapped: stage.appView.runAgain(stage.appView.takeIndex)
            }
            KitChip {
                objectName: "appReuseInputs"
                look: stage.look
                pill: true
                text: qsTr("Reuse inputs")
                glyph: "undo-2"
                visible: !stage.appView.busy && stage.appView.takeIndex >= 0
                tip: qsTr("Put this take's inputs back in the form")
                onTapped: stage.appView.reuseInputs(stage.appView.takeIndex)
            }
            KitChip {
                objectName: "appSaveAs"
                look: stage.look
                pill: true
                glyph: "download"
                visible: stage.current !== null && stage.current.kind !== "text" && !stage.appView.busy
                tip: qsTr("Save a copy as…")
                onTapped: {
                    saver.suggestedName = decodeURIComponent(String(stage.current.full).split("/").pop())
                    saver.open()
                }
            }
            KitChip {
                objectName: "appClear"
                look: stage.look
                pill: true
                text: qsTr("Clear")
                glyph: "x"
                enabled: stage.appView.canClear
                visible: stage.appView.canClear
                tip: qsTr("Clear the results and the takes from this screen; the files stay saved (Results)")
                onTapped: stage.appView.clearResults()
            }
            KitChip {
                objectName: "appMore"
                look: stage.look
                pill: true
                glyph: "ellipsis"
                visible: stage.current !== null && !stage.appView.busy
                tip: qsTr("More")
                onTapped: {
                    const at = mapToItem(Overlay.overlay, 0, height)
                    more.wantX = at.x
                    more.wantY = at.y
                    more.anchorHeight = height
                    more.open()
                }
                WidgetMenu {
                    id: more
                    objectName: "appMoreMenu"
                    look: stage.look
                    parent: Overlay.overlay
                    entries: [
                        { text: qsTr("Open large"), value: "large", enabled: stage.isImage },
                        { text: qsTr("Show in folder"), value: "folder", enabled: stage.current !== null && stage.current.kind !== "text" },
                        { text: qsTr("Use as input"), value: "input", enabled: stage.isImage && stage.hasInputImage },
                        { text: qsTr("Compare with the input"), value: "compare", enabled: stage.canCompare, checked: stage.comparing }
                    ]
                    onPicked: v => {
                        if (v === "large") stage.openLarge()
                        else if (v === "folder") Qt.openUrlExternally(String(stage.current.full).replace(/\/[^/]*$/, "/"))
                        else if (v === "input") stage.notice = stage.appView.useResultAsInput(stage.appView.imageKey(), stage.current.full)
                        else if (v === "compare") stage.toggleCompare()
                    }
                }
            }
        }

        // The words of a failed run, and its Details.
        RowLayout {
            Layout.fillWidth: true
            visible: stage.appView.status === "failed" || stage.notice !== ""
            spacing: 2 * stage.u
            Text {
                objectName: "appError"
                Layout.fillWidth: true
                text: stage.appView.status === "failed" ? stage.appView.error : stage.notice
                color: stage.look.error
                font.pixelSize: stage.look.textSize
                wrapMode: Text.Wrap
            }
            KitCopyButton {
                objectName: "appErrorCopy"
                look: stage.look
                Layout.alignment: Qt.AlignTop
                copyText: stage.errorReport
            }
            KitChip {
                objectName: "appDetails"
                look: stage.look
                pill: true
                text: qsTr("Details")
                glyph: stage.detailsOpen ? "chevron-up" : "chevron-down"
                current: stage.detailsOpen
                visible: stage.appView.status === "failed"
                         && (stage.appView.details.length > 1 || (stage.appView.details.length === 1 && stage.appView.details[0].hint !== ""))
                onTapped: stage.detailsOpen = !stage.detailsOpen
            }
        }
        Column {
            objectName: "appDetailsList"
            Layout.fillWidth: true
            visible: stage.detailsOpen && stage.appView.status === "failed"
            spacing: stage.u
            Repeater {
                model: stage.appView.details
                delegate: Text {
                    required property var modelData
                    width: parent.width
                    text: modelData.hint !== "" ? modelData.message + " " + modelData.hint : modelData.message
                    color: stage.look.label
                    font.pixelSize: stage.look.labelSize
                    wrapMode: Text.Wrap
                }
            }
        }

        // Which result, when the app has more than one.
        RowLayout {
            Layout.fillWidth: true
            visible: stage.results.length > 1
            spacing: stage.u
            Repeater {
                model: stage.results
                delegate: KitChip {
                    required property var modelData
                    required property int index
                    look: stage.look
                    pill: true
                    text: modelData.label
                    current: stage.resultIndex === index
                    onTapped: stage.resultIndex = index
                }
            }
        }

        // The result.
        Rectangle {
            id: well
            objectName: "appResultWell"
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: stage.look.params.cornerWell * stage.look.unit
            color: stage.look.well
            clip: true

            Text {
                objectName: "appEmpty"
                anchors.centerIn: parent
                width: parent.width - 8 * stage.u
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: stage.current === null && !stage.appView.busy
                text: qsTr("Fill in the inputs and press Run. The result appears here, and every run of this session under it.")
                color: stage.look.label
                font.pixelSize: stage.look.textSize
            }
            CompareView {
                id: view
                objectName: "appResultViewer"
                anchors.fill: parent
                look: stage.look
                visible: stage.isImage
                mode: stage.comparing ? "slider" : "single"
                sourceA: stage.isImage ? stage.current.full : ""
                sizeA: stage.isImage ? Qt.size(stage.current.width, stage.current.height) : Qt.size(0, 0)
                labelA: stage.isImage ? stage.current.label : ""
                sourceB: stage.comparing ? stage.inputPicture.url : ""
                sizeB: stage.comparing ? Qt.size(stage.inputPicture.width, stage.inputPicture.height) : Qt.size(0, 0)
                labelB: qsTr("Input")
                // Where the input sits in the result, when the app says (Expand): A shows whole, B at its place in it.
                placeB: stage.comparing && stage.isImage ? stage.appView.inputPlacement(stage.current.width, stage.current.height)
                                                          : Qt.rect(0, 0, 0, 0)
                chrome: stage.comparing
                showZoom: true
                background: "transparent"
                onActivated: if (!stage.comparing) { if (actual || zoom > 1) fit(); else stage.openLarge() }
                Accessible.role: Accessible.Graphic
                Accessible.name: stage.isImage ? stage.current.label : ""
            }
            Loader {
                anchors.fill: parent
                anchors.margins: 2 * stage.u
                active: stage.current !== null && (stage.current.kind === "video" || stage.current.kind === "audio")
                sourceComponent: MediaView {
                    look: stage.look
                    source: stage.current.url
                    kind: stage.current.kind
                    label: stage.current.text
                }
            }
            Text {
                anchors { fill: parent; margins: 3 * stage.u }
                visible: stage.current !== null && stage.current.kind === "text"
                text: stage.current !== null ? stage.current.text : ""
                color: stage.look.text
                font.pixelSize: stage.look.textSize
                wrapMode: Text.Wrap
            }
            // The step picture of a run going, over what was there.
            Rectangle {
                anchors.fill: parent
                visible: stage.appView.busy && (stage.appView.liveFrame !== "" || stage.current === null)
                color: stage.look.well
                Image {
                    objectName: "appLiveFrame"
                    anchors { fill: parent; margins: 2 * stage.u }
                    visible: stage.appView.liveFrame !== ""
                    source: stage.appView.liveFrame
                    fillMode: Image.PreserveAspectFit
                    cache: false
                }
                Text {
                    anchors.centerIn: parent
                    visible: stage.appView.liveFrame === ""
                    text: stage.appView.status === "waiting" ? qsTr("Waiting for its turn…") : qsTr("Working…")
                    color: stage.look.label
                    font.pixelSize: stage.look.textSize
                }
            }
            // A thin line of progress along the top edge of the well, over the picture: in a row of its own above the toolbar it
            // pushed the toolbar down from the card's edge (owner, 2026-11-25).
            Rectangle {
                objectName: "appProgressLine"
                x: well.radius
                width: well.width - 2 * well.radius
                height: 2
                visible: stage.appView.busy
                color: Qt.alpha(stage.look.accent, 0.25)
                Rectangle {
                    width: parent.width * Math.max(stage.appView.status === "waiting" ? 0 : 0.02, stage.appView.progress)
                    height: parent.height
                    color: stage.look.accent
                }
            }
        }

        // The size and zoom, quietly; Fit and 100% at the end.
        RowLayout {
            Layout.fillWidth: true
            spacing: stage.u
            Text {
                objectName: "appSizeLine"
                Layout.fillWidth: true
                text: stage.isImage ? view.sizeNotice : ""
                color: stage.look.label
                font.pixelSize: stage.look.labelSize
                elide: Text.ElideRight
                font.features: { "tnum": 1 }
            }
            KitChip {
                objectName: "appCompare"
                look: stage.look
                pill: true
                text: qsTr("Compare")
                glyph: "columns-2"
                current: stage.comparing
                visible: stage.canCompare || stage.comparing
                tip: qsTr("Compare with the input picture (C)")
                onTapped: stage.toggleCompare()
            }
            KitChip {
                objectName: "appFit"
                look: stage.look
                pill: true
                text: qsTr("Fit")
                glyph: "maximize-2"
                visible: stage.isImage
                tip: qsTr("Fit the picture (Home)")
                onTapped: view.fit()
            }
            KitChip {
                objectName: "appActual"
                look: stage.look
                pill: true
                text: qsTr("100%")
                visible: stage.isImage
                tip: qsTr("One picture pixel to one screen pixel (Z)")
                onTapped: view.actualSize()
            }
        }

        AppTakes {
            Layout.fillWidth: true
            visible: stage.appView.takes.length > 0
            appView: stage.appView
            look: stage.look
        }
        Text {
            objectName: "appTakeInputs"
            Layout.fillWidth: true
            visible: text !== ""
            text: stage.appView.shownInputs
            color: stage.look.label
            font.pixelSize: stage.look.captionSize
            elide: Text.ElideRight
            ToolTip.visible: inputsHover.hovered && truncated
            ToolTip.delay: stage.look.tooltipDelay
            ToolTip.text: text
            HoverHandler { id: inputsHover }
        }
    }

    FilePicker {
        id: saver
        objectName: "appSavePicker"
        title: qsTr("Save the result")
        fileMode: PortalFileDialog.SaveFile
        onAccepted: stage.notice = stage.appView.saveResult(stage.current.full, selectedFile)
    }
    AppResultDialog { id: largeView }
}
