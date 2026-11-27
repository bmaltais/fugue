pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "../media"

// The run queue as an area's view (#648, spec 12.16, as ComfyUI's queue sidebar; Blender's Timeline and Info editors are areas
// too): the runs going and waiting, then the last few that ended. It was the floating queue panel of the run toolbar (#60).
// The rows are App.runQueue's (the runs of this window and of a `fugue run` from a terminal; the shapes of run/run_queue.h);
// what a person asks of a run goes back to it. One per window: panel.queue (Alt+Q) splits it off the bottom of the active
// area and closes it again. The queue reads the going run's events and the history only while this view is made.
//
//   going and waiting: status, name, the node going and its progress, time since it began or was
//     queued, "move to front" (waiting runs but the first) and "cancel"
//   history: how it ended, a thumbnail of the first thing it made (a glyph for a video or a sound),
//     what to call it (its prompt, else the first output's file name, else the workflow's name), how
//     it ended, how long it took, when and the workflow's name, "show results" (a click on the row
//     too: the media browser opens on that run) and "open the run folder"
AreaView {
    id: panel
    objectName: "queueArea"
    readonly property CanvasLook look: App.theme.look
    readonly property var entries: App.runQueue ? App.runQueue.entries : []
    readonly property var history: App.runQueue ? App.runQueue.history : []
    // The image:// source of a thumbnail of a file (the media library's); "" shows no picture.
    function thumbOf(path) {
        return App.mediaLibrary ? App.mediaLibrary.thumbSource(path, 64) : ""
    }
    // What a finished row's thumbnail shows, as the media browser's items do.
    function leadOf(row) {
        if (!row.lead)
            return null
        return {kind: row.leadKind, title: row.label, thumb: thumbOf(row.lead)}
    }
    // The rows' run ids, changed only when the runs or their order change, so a row's buttons
    // stay as they are while its progress and time move.
    property var entryIds: []
    property var historyIds: []
    onEntriesChanged: entryIds = same(entryIds, entries)
    onHistoryChanged: historyIds = same(historyIds, history)
    function same(ids, rows) {
        const wanted = rows.map(r => r.run)
        return wanted.join("\n") === ids.join("\n") ? ids : wanted
    }
    readonly property var noRow: ({run: "", name: "", workflow: "", status: "waiting", position: 0, source: "cli", label: "", lead: "", leadKind: "",
                                   since: 0, node: "", progress: -1, seconds: -1, ended: 0})
    readonly property real u: look.unit
    // Milliseconds since the epoch, read each second while the panel is shown.
    property real now: Date.now()

    Component.onCompleted: {
        // The change handlers above only hear later changes: the rows it is made with are taken here.
        entryIds = same(entryIds, entries)
        historyIds = same(historyIds, history)
    }
    // The first control that can be pressed, for a Queue opened from the keys (the window's `watching` follows the layout, Main.qml).
    function focusFirst() {
        forceActiveFocus()
        step(true)
    }

    function clock(seconds) {
        const s = Math.max(0, Math.floor(seconds))
        const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), r = s % 60
        return h > 0 ? h + ":" + String(m).padStart(2, "0") + ":" + String(r).padStart(2, "0")
                     : m + ":" + String(r).padStart(2, "0")
    }
    function ago(ms) {
        const s = Math.max(0, (now - ms) / 1000)
        if (s < 60)
            return qsTr("just now")
        const m = Math.floor(s / 60)
        if (m < 60)
            return qsTr("%1 min ago").arg(m)
        const h = Math.floor(m / 60)
        return h < 24 ? qsTr("%1 h ago").arg(h) : qsTr("%n days ago", "", Math.floor(h / 24))
    }

    // Up and Down move along the buttons (Tab is the window's own key).
    Keys.onUpPressed: event => step(false)
    Keys.onDownPressed: event => step(true)
    // Focus moves to the next control of the panel; at its ends it stays.
    function step(forward) {
        const from = Window.window ? Window.window.activeFocusItem : null
        const next = from ? from.nextItemInFocusChain(forward) : null
        for (let item = next; item; item = item.parent)
            if (item === panel) {
                next.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
                return
            }
    }
    Timer {
        interval: 1000
        repeat: true
        running: panel.visible
        onTriggered: panel.now = Date.now()
    }

    // The area's own ground, the Canvas area's (nothing else draws behind an area).
    Rectangle {
        anchors.fill: parent
        color: panel.look.popup
    }
    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 2 * panel.u
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: FugueScrollBar { look: panel.look }
        Column {
            id: column
            width: flick.width - (flick.ScrollBar.vertical as FugueScrollBar).inset
            spacing: 0

            // Going and waiting.
            Text {
                objectName: "runQueueHeading"
                width: parent.width
                height: 6 * panel.u
                leftPadding: panel.u
                verticalAlignment: Text.AlignVCenter
                text: qsTr("Running and waiting")
                color: panel.look.label
                font.pixelSize: panel.look.captionSize
                font.weight: Font.DemiBold
            }
            Text {
                visible: panel.entries.length === 0
                objectName: "runQueueEmpty"
                width: parent.width
                height: 7 * panel.u
                leftPadding: panel.u
                verticalAlignment: Text.AlignVCenter
                text: qsTr("Nothing is running.")
                color: panel.look.label
                font.pixelSize: panel.look.labelSize
            }
            Repeater {
                model: panel.entryIds
                delegate: Item {
                    id: entry
                    required property string modelData
                    readonly property var row: panel.entries.find(e => e.run === modelData) ?? panel.noRow
                    readonly property bool running: row.status === "running"
                    objectName: "queueEntry-" + modelData
                    width: column.width
                    height: 9 * panel.u
                    Rectangle {
                        anchors.fill: parent
                        radius: Math.min(panel.look.params.cornerChip * panel.u, 0.45 * height)
                        color: hoverEntry.hovered ? panel.look.menuRow : "transparent"
                    }
                    HoverHandler { id: hoverEntry }
                    Item {
                        id: mark
                        x: panel.u
                        width: 5 * panel.u
                        height: parent.height
                        Glyph {
                            visible: entry.running
                            anchors.centerIn: parent
                            look: panel.look
                            name: "loader-circle"
                            color: panel.look.accent
                            RotationAnimator on rotation {
                                running: entry.running && panel.visible
                                loops: Animation.Infinite
                                from: 0
                                to: 360
                                duration: 900
                            }
                        }
                        Text {
                            visible: !entry.running
                            anchors.centerIn: parent
                            text: entry.row.position
                            color: panel.look.label
                            font.pixelSize: panel.look.labelSize
                            font.features: { "tnum": 1 }
                        }
                    }
                    Column {
                        x: mark.x + mark.width + panel.u
                        width: parent.width - x - buttons.width - elapsed.width - 3 * panel.u
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            objectName: "queueName"
                            width: parent.width
                            text: entry.row.name
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            color: panel.look.text
                            font.pixelSize: panel.look.labelSize
                            font.weight: Font.DemiBold
                        }
                        Text {
                            objectName: "queueDetail"
                            width: parent.width
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: panel.look.label
                            font.pixelSize: panel.look.captionSize
                            text: {
                                const m = entry.row
                                const from = m.source === "cli" ? qsTr("from the command line") : qsTr("from the window")
                                if (!entry.running)
                                    return qsTr("Waiting, %1").arg(from)
                                const what = m.node !== "" ? m.node + (m.progress >= 0 ? " " + Math.round(m.progress * 100) + "%" : "")
                                                           : qsTr("Starting")
                                return what + " · " + from
                            }
                        }
                    }
                    Text {
                        id: elapsed
                        objectName: "queueElapsed"
                        anchors.right: buttons.left
                        anchors.rightMargin: panel.u
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.clock((panel.now - entry.row.since) / 1000)
                        color: panel.look.label
                        font.pixelSize: panel.look.captionSize
                        font.features: { "tnum": 1 }
                    }
                    Row {
                        id: buttons
                        anchors.right: parent.right
                        anchors.rightMargin: panel.u
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: panel.u
                        ViewButton {
                            objectName: "queueFront-" + entry.modelData
                            look: panel.look
                            glyph: "arrow-up"
                            tip: qsTr("Move to the front of the queue")
                            visible: !entry.running
                            enabled: entry.row.position > 1
                            onTapped: App.runQueue.moveToFront(entry.modelData)
                        }
                        ViewButton {
                            objectName: "queueCancel-" + entry.modelData
                            look: panel.look
                            danger: true
                            glyph: "x"
                            tip: entry.running ? qsTr("Cancel this run") : qsTr("Take this run out of the queue")
                            onTapped: App.runQueue.cancel(entry.modelData)
                        }
                    }
                    Rectangle {
                        visible: entry.running && entry.row.progress >= 0
                        x: panel.u
                        y: parent.height - 2
                        width: Math.max(0, (parent.width - 2 * panel.u) * entry.row.progress)
                        height: 2
                        color: panel.look.accent
                    }
                }
            }

            // History.
            Item { width: 1; height: panel.u }
            Rectangle {
                width: parent.width
                height: 1
                color: panel.look.hairline
            }
            Text {
                width: parent.width
                height: 6 * panel.u
                leftPadding: panel.u
                verticalAlignment: Text.AlignVCenter
                text: qsTr("Finished")
                color: panel.look.label
                font.pixelSize: panel.look.captionSize
                font.weight: Font.DemiBold
            }
            Text {
                visible: panel.history.length === 0
                objectName: "runHistoryEmpty"
                width: parent.width
                height: 7 * panel.u
                leftPadding: panel.u
                verticalAlignment: Text.AlignVCenter
                text: qsTr("Finished runs show here.")
                color: panel.look.label
                font.pixelSize: panel.look.labelSize
            }
            Repeater {
                model: panel.historyIds
                delegate: Item {
                    id: done
                    required property string modelData
                    readonly property var row: panel.history.find(e => e.run === modelData) ?? panel.noRow
                    objectName: "historyEntry-" + modelData
                    width: column.width
                    height: 9 * panel.u
                    Rectangle {
                        anchors.fill: parent
                        radius: Math.min(panel.look.params.cornerChip * panel.u, 0.45 * height)
                        color: hoverDone.hovered ? panel.look.menuRow : "transparent"
                    }
                    HoverHandler { id: hoverDone }
                    // A click on the row shows what the run made.
                    TapHandler { onTapped: App.runQueue.showResults(done.modelData) }
                    Glyph {
                        id: outcome
                        x: panel.u + (5 * panel.u - width) / 2
                        anchors.verticalCenter: parent.verticalCenter
                        look: panel.look
                        name: done.row.status === "ok" ? "check" : done.row.status === "failed" ? "circle-x" : "ban"
                        color: done.row.status === "failed" ? panel.look.error : panel.look.label
                    }
                    // The first thing the run made; room is kept when it made nothing, so the names line up.
                    MediaThumb {
                        id: lead
                        objectName: "historyLead-" + done.modelData
                        x: 7 * panel.u
                        anchors.verticalCenter: parent.verticalCenter
                        width: 7 * panel.u
                        height: 7 * panel.u
                        look: panel.look
                        compact: true
                        item: panel.leadOf(done.row)
                        visible: item !== null
                    }
                    Column {
                        x: 15 * panel.u
                        width: parent.width - x - doneButtons.width - 2 * panel.u
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            objectName: "historyName"
                            width: parent.width
                            text: done.row.label !== "" ? done.row.label : done.row.name
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            color: panel.look.text
                            font.pixelSize: panel.look.labelSize
                            font.weight: Font.DemiBold
                        }
                        Text {
                            objectName: "historyDetail"
                            width: parent.width
                            elide: Text.ElideRight
                            color: panel.look.label
                            font.pixelSize: panel.look.captionSize
                            text: {
                                const m = done.row
                                const how = m.status === "ok" ? qsTr("Done") : m.status === "failed" ? qsTr("Failed") : qsTr("Cancelled")
                                return how + (m.seconds >= 0 ? " · " + panel.clock(m.seconds) : "") + " · " + panel.ago(m.ended)
                                       + (m.label !== "" ? " · " + m.name : "")
                            }
                        }
                    }
                    Row {
                        id: doneButtons
                        anchors.right: parent.right
                        anchors.rightMargin: panel.u
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: panel.u
                        ViewButton {
                            objectName: "historyResults-" + done.modelData
                            look: panel.look
                            glyph: "image"
                            tip: qsTr("Show its results")
                            onTapped: App.runQueue.showResults(done.modelData)
                        }
                        ViewButton {
                            objectName: "historyFolder-" + done.modelData
                            look: panel.look
                            glyph: "folder"
                            tip: qsTr("Open its run folder")
                            onTapped: App.runQueue.openFolder(done.modelData)
                        }
                    }
                }
            }
        }
    }
}
