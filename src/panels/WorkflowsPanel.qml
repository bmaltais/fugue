pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "../media"

// The workflows panel (#56, ComfyUI's Workflows tab): the workflows open in tabs (a click shows one,
// × closes it), then the files opened or saved lately that are not open (a click opens one). The
// library of groups (#26) will be a third list here. It shows the caller's `tabs` (the maps of
// Workflows::tabs) and `recent` (RecentFiles::entries) and says what a person asked; it knows
// nothing of files or of the canvas.
//
// A switch at the top changes the view from the files to the results (#133): the images, videos and
// sounds the current workflow's runs made, in a MediaBrowser. The caller supplies `results` when
// `resultsRequested` says the view needs them (it was opened, the workflow changed, or the caller
// knows a run ended), `details` of the browser's current item when `resultChosen` says it changed,
// and does what `resultAction` asks.
//
//   Up and Down move along the rows, Enter does a row's action, Delete or Ctrl+W closes an open
//   workflow, Escape closes the panel
Item {
    id: root
    objectName: "workflowsPanel"
    required property CanvasLook look
    property var tabs: []
    property var recent: []
    property string view: "files" // "files" or "results"
    property var results: []
    property var resultDetails: []
    property string resultsMode: "grid"
    // For SidePanel.focusTarget.
    readonly property Item focusTarget: view === "results" && resultsLoader.item ? (resultsLoader.item as Item) : list
    // The current workflow's file and name; "" for one never saved.
    readonly property var currentTab: tabs.find(t => t.current) ?? null
    readonly property string workflowPath: currentTab?.path ?? ""

    signal tabRequested(int index)
    signal tabCloseRequested(int index)
    signal fileRequested(string path)
    signal closeRequested
    signal resultsRequested
    signal resultChosen(var item)
    signal resultAction(string action, var item)

    readonly property real u: look.unit
    property alias rowList: list
    readonly property real switchHeight: 7 * u

    onViewChanged: if (view === "results") resultsRequested()
    onWorkflowPathChanged: if (view === "results") resultsRequested()

    // The rows: a heading, then its entries, for each of the two lists. A heading is never current.
    readonly property var rows: {
        const out = [{kind: "heading", title: qsTr("Open")}]
        tabs.forEach((t, i) => out.push({kind: "tab", index: i, title: t.title, path: t.path, unsaved: t.unsaved,
                                         current: t.current, color: t.color}))
        out.push({kind: "heading", title: qsTr("Recent")})
        if (recent.length === 0)
            out.push({kind: "empty", title: qsTr("Files you open or save show here.")})
        recent.forEach(r => out.push({kind: "recent", title: r.name, path: r.path, folder: r.folder}))
        return out
    }

    function actionable(i) {
        const kind = rows[i]?.kind
        return kind === "tab" || kind === "recent"
    }
    function step(by) {
        let i = list.currentIndex + by
        while (i >= 0 && i < rows.length && !actionable(i))
            i += by
        if (i >= 0 && i < rows.length) {
            list.currentIndex = i
            list.positionViewAtIndex(i, ListView.Contain)
        }
    }
    function activate(i) {
        const row = rows[i]
        if (row?.kind === "tab")
            tabRequested(row.index)
        else if (row?.kind === "recent")
            fileRequested(row.path)
    }

    // Files or results: two buttons, one chunk, the same height as the browser's.
    Row {
        id: viewSwitch
        objectName: "workflowsViewSwitch"
        x: root.u
        y: root.u
        spacing: root.u
        ViewButton {
            objectName: "workflowsView-files"
            look: root.look
            text: qsTr("Files")
            current: root.view === "files"
            onTapped: root.view = "files"
        }
        ViewButton {
            objectName: "workflowsView-results"
            look: root.look
            text: qsTr("Results")
            tip: qsTr("The images, videos and sounds this workflow made")
            current: root.view === "results"
            onTapped: root.view = "results"
        }
        ViewButton {
            objectName: "workflowsResultsRefresh"
            look: root.look
            glyph: "refresh-cw"
            tip: qsTr("Look for new results")
            visible: root.view === "results"
            onTapped: root.resultsRequested()
        }
    }
    // Made only while the results are shown (the item budget).
    Loader {
        id: resultsLoader
        active: root.view === "results"
        anchors { left: parent.left; right: parent.right; top: viewSwitch.bottom; bottom: parent.bottom; topMargin: root.u }
        sourceComponent: MediaBrowser {
            objectName: "workflowsResults"
            look: root.look
            items: root.results
            details: root.resultDetails
            mode: root.resultsMode
            actions: [{id: "open", label: qsTr("Open")}, {id: "folder", label: qsTr("Show folder")},
                      {id: "copy", label: qsTr("Copy path")}]
            emptyText: root.workflowPath === "" ? qsTr("Save the workflow, then run it: its results show here.")
                                                : qsTr("Run this workflow: its images, videos and sounds show here.")
            onModeChanged: root.resultsMode = mode
            onCurrentItemChanged: if (currentItem) root.resultChosen(currentItem)
            onActionRequested: (action, item) => root.resultAction(action, item)
            onCloseRequested: root.closeRequested()
            Component.onCompleted: forceActiveFocus()
        }
    }

    ListView {
        id: list
        objectName: "workflowsList"
        visible: root.view === "files"
        anchors { fill: parent; topMargin: root.switchHeight + 2 * root.u; bottomMargin: root.u }
        clip: true
        model: root.rows
        currentIndex: -1
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: true
        ScrollBar.vertical: FugueScrollBar { look: root.look }
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Workflows")
        onActiveFocusChanged: if (activeFocus && currentIndex < 0) root.step(1)
        Keys.onDownPressed: root.step(1)
        Keys.onUpPressed: root.step(-1)
        Keys.onReturnPressed: root.activate(currentIndex)
        Keys.onEnterPressed: root.activate(currentIndex)
        Keys.onEscapePressed: root.closeRequested()
        Keys.onDeletePressed: {
            const row = root.rows[currentIndex]
            if (row?.kind === "tab")
                root.tabCloseRequested(row.index)
        }

        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property string kind: modelData.kind
            readonly property bool pressable: kind === "tab" || kind === "recent"
            readonly property real edge: list.ScrollBar.vertical.visible ? list.ScrollBar.vertical.width : 0
            width: list.width
            height: kind === "heading" ? 6 * root.u : kind === "empty" ? 7 * root.u : 8 * root.u

            Rectangle {
                visible: row.pressable
                x: root.u
                width: parent.width - 2 * root.u - row.edge
                height: parent.height
                radius: root.look.smallCorner
                color: list.activeFocus && list.currentIndex === row.index ? root.look.selectedRow
                       : hover.hovered ? root.look.menuRow
                       : row.modelData.current ? root.look.well : "transparent"
            }
            // The tab's own color, as along the tab strip.
            Rectangle {
                visible: row.kind === "tab"
                x: 2 * root.u
                y: 1.5 * root.u
                width: 2
                height: parent.height - 3 * root.u
                radius: 1
                color: row.kind === "tab" ? (root.look.tabColors[row.modelData.color] ?? root.look.accent) : "transparent"
            }
            Text {
                visible: row.kind === "heading" || row.kind === "empty"
                x: 3 * root.u
                width: parent.width - 6 * root.u
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.title
                wrapMode: row.kind === "empty" ? Text.Wrap : Text.NoWrap
                color: root.look.label
                font.pixelSize: row.kind === "heading" ? root.look.captionSize : root.look.labelSize
                font.weight: row.kind === "heading" ? Font.DemiBold : Font.Normal
            }
            // An open workflow: its name, an unsaved dot, and the close button (the row's whole
            // height and 7u wide, past the floor).
            Text {
                visible: row.kind === "tab"
                x: 5 * root.u
                width: unsavedDot.x - x - root.u
                anchors.verticalCenter: parent.verticalCenter
                text: row.kind === "tab" ? row.modelData.title : ""
                elide: Text.ElideRight
                color: root.look.text
                font.pixelSize: root.look.labelSize
                font.weight: row.kind === "tab" && row.modelData.current ? Font.DemiBold : Font.Normal
            }
            Rectangle {
                id: unsavedDot
                objectName: "workflowsUnsaved"
                visible: row.kind === "tab" && row.modelData.unsaved
                x: closeButton.x - width - root.u
                anchors.verticalCenter: parent.verticalCenter
                width: 1.5 * root.u
                height: width
                radius: width / 2
                color: root.look.accent
                Accessible.name: qsTr("Unsaved")
            }
            // A recent file: its name, and under it the folder.
            Column {
                visible: row.kind === "recent"
                x: 3 * root.u
                width: parent.width - 6 * root.u - row.edge
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    width: parent.width
                    text: row.kind === "recent" ? row.modelData.title : ""
                    elide: Text.ElideRight
                    color: root.look.text
                    font.pixelSize: root.look.labelSize
                }
                Text {
                    width: parent.width
                    text: row.kind === "recent" ? row.modelData.folder : ""
                    elide: Text.ElideMiddle
                    color: root.look.label
                    font.pixelSize: root.look.captionSize
                }
            }
            HoverHandler {
                id: hover
                enabled: row.pressable
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                enabled: row.pressable
                onTapped: eventPoint => {
                    // The close button is its own target.
                    if (row.kind === "tab" && eventPoint.position.x >= closeButton.x)
                        return
                    list.currentIndex = row.index
                    list.forceActiveFocus()
                    root.activate(row.index)
                }
            }
            Item {
                id: closeButton
                objectName: "workflowsClose-" + row.index
                visible: row.kind === "tab"
                x: parent.width - width - 2 * root.u - row.edge
                width: 7 * root.u
                height: parent.height
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Close workflow")
                Glyph {
                    anchors.centerIn: parent
                    name: "x"
                    look: root.look
                    color: closeHover.hovered ? root.look.text : root.look.label
                }
                HoverHandler {
                    id: closeHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: root.tabCloseRequested(row.modelData.index)
                }
            }
            ToolTip.visible: hover.hovered && row.pressable && row.modelData.path !== ""
            ToolTip.text: row.modelData.path ?? ""
            ToolTip.delay: 600
            Accessible.role: Accessible.ListItem
            Accessible.name: row.modelData.title ?? ""
        }
    }
}
