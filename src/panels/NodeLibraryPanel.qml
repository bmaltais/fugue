pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "../kit"

// The node library (#56, ComfyUI's Node Library tab): a search field over the node types by
// category. It shows the caller's `rows` (the shape of search/node_library.h: category, depth, key,
// label, count, open, description) and says what a person asked: open or close a category, add a
// node (a click, or Enter), drop a node (dragged out of the panel, with the window point it was
// let go at). Where the node goes and what the query matches are the caller's; this knows nothing
// of the canvas.
//
//   click or Enter on a category: opens it, Left and Right close and open it; on a node: adds it
//   a group of the user's library (`library` in its row) also has a trash can at its far end, and
//   Delete on its row does the same: both ask first (DeleteGroupQuestion) and say `groupDeleted`
//   only after a yes; the caller deletes it, and the new rows follow
//   Down from the field goes to the list, Up from its first row back; Escape empties the field,
//   then closes the panel
Item {
    id: root
    objectName: "nodeLibrary"
    required property CanvasLook look
    property var rows: []
    property string query
    property int nodeCount: 0
    // For SidePanel.focusTarget.
    readonly property Item focusTarget: field

    signal queryEdited(string query)
    signal categoryToggled(string key)
    signal nodeActivated(string typeId)
    signal nodeDropped(string typeId, real sceneX, real sceneY)
    signal groupDeleted(string typeId, string name)
    signal closeRequested

    readonly property real u: look.unit
    property alias rowList: list
    property alias searchField: field
    property alias deleteQuestion: deleteAsk

    // The row that takes Enter and the arrows, -1 for none.
    function addCurrent() {
        const row = rows[list.currentIndex]
        if (!row)
            return
        if (row.category)
            categoryToggled(row.key)
        else
            nodeActivated(row.key)
    }

    // What a trash can, or Delete on a library row, asks (#156).
    function askDelete(row) {
        if (row && row.library)
            deleteAsk.ask(row.key, row.label)
    }

    DeleteGroupQuestion {
        id: deleteAsk
        onConfirmed: (groupId, groupName) => root.groupDeleted(groupId, groupName)
        onClosed: list.forceActiveFocus()
    }

    // Search field: a glyph, the query, a clear button while there is one.
    Item {
        id: fieldBar
        x: 2 * root.u
        y: root.u
        width: parent.width - 4 * root.u
        height: 8 * root.u
        KitWell { look: root.look; focused: field.activeFocus } // the recessed field of the kit
        Glyph {
            id: searchGlyph
            x: 2 * root.u
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            look: root.look
            color: root.look.label
        }
        TextInput {
            id: field
            objectName: "nodeLibrarySearch"
            x: searchGlyph.x + searchGlyph.width + 2 * root.u
            width: clearButton.x - x - root.u
            anchors.verticalCenter: parent.verticalCenter
            clip: true
            text: root.query
            color: root.look.text
            selectionColor: root.look.accent
            selectedTextColor: root.look.accentForeground
            selectByMouse: true
            font.pixelSize: root.look.searchFieldSize
            Accessible.name: qsTr("Search nodes")
            Text {
                visible: field.text === "" && !field.preeditText
                text: qsTr("Search nodes")
                color: root.look.label
                font: field.font
            }
            onTextEdited: root.queryEdited(text)
            Keys.onDownPressed: {
                list.currentIndex = Math.max(0, list.currentIndex)
                list.forceActiveFocus()
            }
            // Enter in the field adds the best match: the first node (a category is only a heading).
            Keys.onReturnPressed: {
                const first = root.rows.findIndex(r => !r.category)
                if (first >= 0) {
                    list.currentIndex = first
                    root.addCurrent()
                }
            }
            Keys.onEscapePressed: event => {
                if (text !== "")
                    root.queryEdited("")
                else
                    root.closeRequested()
            }
        }
        // The clear button: the whole 8u height is the target, shown only with a query.
        Item {
            id: clearButton
            objectName: "nodeLibraryClear"
            visible: field.text !== ""
            x: parent.width - width
            width: parent.height
            height: parent.height
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Clear search")
            Glyph {
                anchors.centerIn: parent
                name: "x"
                look: root.look
                color: root.look.label
            }
            TapHandler {
                gesturePolicy: TapHandler.WithinBounds
                onTapped: {
                    root.queryEdited("")
                    field.forceActiveFocus()
                }
            }
        }
    }

    ListView {
        id: list
        objectName: "nodeLibraryList"
        anchors { left: parent.left; right: parent.right; top: fieldBar.bottom; bottom: parent.bottom; topMargin: root.u; bottomMargin: root.u }
        clip: true
        model: root.rows
        currentIndex: -1
        // A drag on a row takes the node out of the panel, so the list scrolls by the wheel and
        // the scroll bar, not by dragging.
        interactive: false
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: true
        keyNavigationEnabled: false
        ScrollBar.vertical: FugueScrollBar { look: root.look }
        TipGate { id: tipGate; view: list }
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Nodes")

        function step(by) {
            if (root.rows.length === 0)
                return
            if (by < 0 && currentIndex <= 0) {
                field.forceActiveFocus()
                return
            }
            currentIndex = Math.max(0, Math.min(root.rows.length - 1, currentIndex + by))
            positionViewAtIndex(currentIndex, ListView.Contain)
        }
        Keys.onDownPressed: step(1)
        Keys.onUpPressed: step(-1)
        Keys.onReturnPressed: root.addCurrent()
        Keys.onEnterPressed: root.addCurrent()
        Keys.onEscapePressed: root.closeRequested()
        // Delete on a library group's row asks to delete it; the window's own Delete (the selected
        // nodes) is not taken while the row is not a library group.
        Keys.onDeletePressed: root.askDelete(root.rows[currentIndex])
        Keys.onShortcutOverride: event => {
            if (event.key === Qt.Key_Delete && root.rows[currentIndex]?.library)
                event.accepted = true
        }
        Keys.onRightPressed: {
            const row = root.rows[currentIndex]
            if (row && row.category && !row.open)
                root.categoryToggled(row.key)
        }
        Keys.onLeftPressed: {
            const row = root.rows[currentIndex]
            if (row && row.category && row.open)
                root.categoryToggled(row.key)
        }
        // Rows change when a category opens or the query is typed: keep the same row current.
        onModelChanged: if (currentIndex >= root.rows.length) currentIndex = root.rows.length - 1

        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property bool category: modelData.category
            width: list.width
            height: 7 * root.u
            Rectangle {
                x: root.u
                width: parent.width - 2 * root.u - (list.ScrollBar.vertical as FugueScrollBar).inset
                height: parent.height
                radius: Math.min(root.look.params.cornerChip * root.u, 0.45 * height)
                color: list.activeFocus && list.currentIndex === row.index ? root.look.selectedRow
                       : hover.hovered ? root.look.menuRow : "transparent"
            }
            Glyph {
                id: chevron
                visible: row.category
                x: 2 * root.u + row.modelData.depth * 3 * root.u
                anchors.verticalCenter: parent.verticalCenter
                name: row.modelData.open ? "chevron-down" : "chevron-right"
                look: root.look
                color: root.look.label
            }
            Text {
                x: chevron.x + 4.5 * root.u
                width: (trash.item ? trash.x : countText.x) - x - root.u
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.label
                elide: Text.ElideRight
                color: row.category ? root.look.label : root.look.text
                font.pixelSize: root.look.labelSize
                font.weight: row.category ? Font.DemiBold : Font.Normal
            }
            Text {
                id: countText
                visible: row.category
                x: parent.width - width - 3 * root.u - (list.ScrollBar.vertical as FugueScrollBar).inset
                anchors.verticalCenter: parent.verticalCenter
                text: row.category ? row.modelData.count : ""
                color: root.look.label
                font.pixelSize: root.look.captionSize
            }
            // The trash can of a library group, at the row's far end and clear of the scroll bar; made
            // only on those rows.
            Loader {
                id: trash
                active: row.modelData.library === true
                x: parent.width - root.u - width - (list.ScrollBar.vertical as FugueScrollBar).inset
                anchors.verticalCenter: parent.verticalCenter
                width: 7 * root.u
                height: width
                sourceComponent: RowTrashButton {
                    objectName: "nodeLibraryTrash"
                    look: root.look
                    label: qsTr("Delete %1 from the library").arg(row.modelData.label)
                    ToolTip.visible: hovered && tipGate.open
                    ToolTip.delay: App.theme.look.tooltipDelay
                    ToolTip.text: label
                    onClicked: {
                        list.currentIndex = row.index
                        root.askDelete(row.modelData)
                    }
                }
            }
            HoverHandler {
                id: hover
                cursorShape: Qt.PointingHandCursor
            }
            // A press on the trash can's square is its own: the row neither adds nor drags from it.
            function inTrash(point) {
                return trash.active && point.x >= trash.x && point.x <= trash.x + trash.width
            }
            // Passive on purpose: it shares the press with the row's DragHandler (a node dragged out to the canvas).
            TapHandler {
                gesturePolicy: TapHandler.DragThreshold
                onTapped: eventPoint => {
                    if (row.inTrash(eventPoint.position))
                        return
                    list.currentIndex = row.index
                    list.forceActiveFocus()
                    root.addCurrent()
                }
            }
            // A node dragged out lets go over the canvas: the caller adds it where it was dropped.
            DragHandler {
                id: drag
                target: null
                enabled: !row.category
                // The press was on the trash can: a drag from there takes nothing out.
                property bool fromTrash: false
                onActiveChanged: {
                    if (active)
                        fromTrash = row.inTrash(centroid.pressPosition)
                    if (fromTrash) {
                        ghost.visible = false
                        return
                    }
                    ghost.visible = active
                    if (active)
                        ghost.text = row.modelData.label
                    else
                        root.nodeDropped(row.modelData.key, centroid.scenePosition.x, centroid.scenePosition.y)
                }
                onCentroidChanged: if (active && !fromTrash) ghost.follow(centroid.scenePosition)
            }
            ToolTip.visible: hover.hovered && tipGate.open && !row.category && row.modelData.description !== ""
            ToolTip.delay: App.theme.look.tooltipDelay
            ToolTip.text: row.modelData.description
            Accessible.role: Accessible.ListItem
            Accessible.name: row.modelData.label
        }
    }

    Text {
        objectName: "nodeLibraryEmpty"
        visible: root.rows.length === 0
        anchors { left: parent.left; right: parent.right; top: fieldBar.bottom; margins: 3 * root.u; topMargin: 4 * root.u }
        wrapMode: Text.Wrap
        text: root.query !== "" ? qsTr("No node matches “%1”.").arg(root.query) : qsTr("No nodes are installed.")
        color: root.look.label
        font.pixelSize: root.look.labelSize
    }

    // What follows the pointer while a node is dragged out: its name on a chip, over everything.
    Rectangle {
        id: ghost
        objectName: "nodeLibraryGhost"
        property alias text: ghostText.text
        parent: Overlay.overlay
        visible: false
        z: 1000
        width: ghostText.implicitWidth + 4 * root.u
        height: 7 * root.u
        radius: Math.min(root.look.params.cornerChip * root.u, 0.45 * height)
        color: root.look.popup
        border.width: 1
        border.color: root.look.accent
        opacity: 0.92
        function follow(scene) {
            x = scene.x + root.u
            y = scene.y + root.u
        }
        Text {
            id: ghostText
            anchors.centerIn: parent
            color: root.look.text
            font.pixelSize: root.look.labelSize
        }
    }
}
