pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"

// The node library (#56, ComfyUI's Node Library tab): a search field over the node types by
// category. It shows the caller's `rows` (the shape of search/node_library.h: category, depth, key,
// label, count, open, description) and says what a person asked: open or close a category, add a
// node (a click, or Enter), drop a node (dragged out of the panel, with the window point it was
// let go at). Where the node goes and what the query matches are the caller's; this knows nothing
// of the canvas.
//
//   click or Enter on a category: opens it, Left and Right close and open it; on a node: adds it
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
    signal closeRequested

    readonly property real u: look.unit
    property alias rowList: list
    property alias searchField: field

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

    // Search field: a glyph, the query, a clear button while there is one.
    Item {
        id: fieldBar
        x: 2 * root.u
        y: root.u
        width: parent.width - 4 * root.u
        height: 8 * root.u
        Rectangle {
            anchors.fill: parent
            radius: root.look.smallCorner
            color: root.look.well
            border.width: field.activeFocus ? 1 : 0
            border.color: root.look.accent
        }
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
        ScrollBar.vertical: ScrollBar {
            policy: list.contentHeight > list.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
        }
        WheelHandler {
            onWheel: event => {
                list.contentY = Math.max(0, Math.min(list.contentHeight - list.height, list.contentY - event.angleDelta.y / 2))
            }
        }
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
                width: parent.width - 2 * root.u - (list.ScrollBar.vertical.visible ? list.ScrollBar.vertical.width : 0)
                height: parent.height
                radius: root.look.smallCorner
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
                width: countText.x - x - root.u
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
                x: parent.width - width - 3 * root.u - (list.ScrollBar.vertical.visible ? list.ScrollBar.vertical.width : 0)
                anchors.verticalCenter: parent.verticalCenter
                text: row.category ? row.modelData.count : ""
                color: root.look.label
                font.pixelSize: root.look.captionSize
            }
            HoverHandler {
                id: hover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: {
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
                onActiveChanged: {
                    ghost.visible = active
                    if (active)
                        ghost.text = row.modelData.label
                    else
                        root.nodeDropped(row.modelData.key, centroid.scenePosition.x, centroid.scenePosition.y)
                }
                onCentroidChanged: if (active) ghost.follow(centroid.scenePosition)
            }
            ToolTip.visible: hover.hovered && !row.category && row.modelData.description !== ""
            ToolTip.text: row.modelData.description
            ToolTip.delay: 600
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
        radius: root.look.smallCorner
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
