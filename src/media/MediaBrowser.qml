pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"

// A browser for a list of images, videos and sounds (#133): a grid grouped by run, a filmstrip
// under one large item, or one item large, with the keys of an image browser and a few actions.
// It knows nothing of the canvas, a workflow or a file system: the host gives it the list and says
// what to do with an action, and it says what a person chose. The workflows panel hosts it; a
// node's face and the large view can host it the same way (docs/spec.md 12.14, Appendix A).
//
// Input:  look; items, a list of rows {url, kind ("image"|"video"|"audio"), title, thumb (an
//         image source for the grid, or ""), group (a heading for the rows that follow, or "")};
//         details, a list of {label, value} for the current item (the host fills it when
//         `currentItem` changes); actions, a list of {id, label}; emptyText; mode.
// Output: currentIndex and currentItem; mode ("grid", "filmstrip", "single"); signals
//         actionRequested(id, item), activated(item) (Enter or a double click on an item: the
//         browser has gone to single), closeRequested (Escape in a grid or a filmstrip).
// Keys:   Left and Right step (Up and Down move a row in the grid), Home and End, Enter opens the
//         current item large, Escape goes back to the grid (then asks to close), I shows or hides
//         the details. In a video's or sound's player, Left and Right seek once the player has the
//         focus (a click on it); Escape or a click outside gives the focus back.
// Cost:   the grid and the filmstrip are lists, so only the cells in view exist; the large item and
//         its player exist only outside the grid.
FocusScope {
    id: root
    required property CanvasLook look
    property var items: []
    property var details: []
    property var actions: []
    property string emptyText: qsTr("Nothing here yet.")
    property string mode: "grid"
    property int currentIndex: -1
    property bool showInfo: true

    readonly property var currentItem: currentIndex >= 0 && currentIndex < items.length ? items[currentIndex] : null
    readonly property real u: look.unit
    readonly property real cell: 24 * u // 96px
    readonly property real strip: 18 * u
    readonly property int columns: Math.max(1, Math.floor((width - u) / (cell + u)))
    // The grid's rows: a heading for each group, then the item indexes, `columns` to a row.
    readonly property var gridRows: {
        const out = []
        let group = null
        let row = null
        const per = columns
        items.forEach((item, i) => {
            if (item.group !== group) {
                group = item.group
                row = null
                if (group)
                    out.push({heading: group})
            }
            if (!row || row.cells.length >= per) {
                row = {cells: []}
                out.push(row)
            }
            row.cells.push(i)
        })
        return out
    }
    property string browseMode: "grid" // where Escape goes back to from single

    signal actionRequested(string action, var item)
    signal activated(var item)
    signal closeRequested

    function select(i) {
        if (items.length === 0)
            return
        currentIndex = Math.max(0, Math.min(items.length - 1, i))
        Qt.callLater(root.reveal)
    }
    function rowOf(index) {
        return gridRows.findIndex(r => r.cells && r.cells.includes(index))
    }
    function stepRow(by) {
        let r = rowOf(currentIndex) + by
        while (r >= 0 && r < gridRows.length && !gridRows[r].cells)
            r += by
        if (r < 0 || r >= gridRows.length)
            return
        const column = gridRows[rowOf(currentIndex)].cells.indexOf(currentIndex)
        const cells = gridRows[r].cells
        select(cells[Math.min(column, cells.length - 1)])
    }
    function reveal() {
        if (currentIndex < 0)
            return
        if (mode === "grid")
            gridList.positionViewAtIndex(rowOf(currentIndex), ListView.Contain)
        else
            (filmstripLoader.item as ListView)?.positionViewAtIndex(currentIndex, ListView.Contain)
    }
    function setMode(next) {
        if (next === mode)
            return
        if (next !== "single")
            browseMode = next
        mode = next
        Qt.callLater(root.reveal)
        root.forceActiveFocus()
    }
    function open() {
        if (!currentItem)
            return
        if (mode !== "single")
            setMode("single")
        activated(currentItem)
    }
    function act(id) {
        if (currentItem)
            actionRequested(id, currentItem)
    }

    onItemsChanged: {
        if (items.length === 0)
            currentIndex = -1
        else if (currentIndex < 0)
            currentIndex = 0
        else if (currentIndex >= items.length)
            currentIndex = items.length - 1
    }
    onColumnsChanged: Qt.callLater(root.reveal)

    activeFocusOnTab: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Media")
    // The window's shortcuts (Home fits the view, I and others) must not take the browser's keys.
    Keys.onShortcutOverride: event => {
        if (!(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)))
            event.accepted = [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_PageUp, Qt.Key_PageDown,
                              Qt.Key_Home, Qt.Key_End, Qt.Key_Return, Qt.Key_Enter, Qt.Key_Escape, Qt.Key_I].includes(event.key)
    }
    Keys.onPressed: event => {
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
            return
        const grid = mode === "grid"
        let used = true
        switch (event.key) {
        case Qt.Key_Left: case Qt.Key_PageUp: select(currentIndex - 1); break
        case Qt.Key_Right: case Qt.Key_PageDown: select(currentIndex + 1); break
        case Qt.Key_Up: if (grid) stepRow(-1); else used = false; break
        case Qt.Key_Down: if (grid) stepRow(1); else used = false; break
        case Qt.Key_Home: select(0); break
        case Qt.Key_End: select(items.length - 1); break
        case Qt.Key_Return: case Qt.Key_Enter: open(); break
        case Qt.Key_Escape:
            if (mode === "single") setMode(browseMode)
            else closeRequested()
            break
        case Qt.Key_I: showInfo = !showInfo; break
        default: used = false
        }
        event.accepted = used
    }

    // The bar: the mode, the count, the details switch.
    Flow {
        id: bar
        objectName: "mediaBar"
        x: root.u
        y: root.u
        width: parent.width - 2 * root.u
        spacing: root.u
        ViewButton {
            objectName: "mediaMode-grid"
            look: root.look
            text: qsTr("Grid")
            current: root.mode === "grid"
            onTapped: root.setMode("grid")
        }
        ViewButton {
            objectName: "mediaMode-filmstrip"
            look: root.look
            text: qsTr("Filmstrip")
            current: root.mode === "filmstrip"
            onTapped: root.setMode("filmstrip")
        }
        ViewButton {
            objectName: "mediaMode-single"
            look: root.look
            text: qsTr("Single")
            current: root.mode === "single"
            onTapped: root.setMode("single")
        }
        ViewButton {
            objectName: "mediaInfo"
            look: root.look
            text: qsTr("Info")
            tip: qsTr("Show or hide the details (I)")
            current: root.showInfo
            visible: root.mode !== "grid"
            onTapped: root.showInfo = !root.showInfo
        }
        Text {
            objectName: "mediaCount"
            height: 7 * root.u
            verticalAlignment: Text.AlignVCenter
            text: root.items.length === 0 ? "" : qsTr("%1 of %2").arg(root.currentIndex + 1).arg(root.items.length)
            color: root.look.label
            font.pixelSize: root.look.labelSize
            font.features: { "tnum": 1 }
        }
    }

    // The actions on the current item, under everything.
    Flow {
        id: foot
        objectName: "mediaActions"
        visible: root.actions.length > 0
        x: root.u
        width: parent.width - 2 * root.u
        y: parent.height - (visible ? height : 0) - root.u
        spacing: root.u
        Repeater {
            model: root.actions
            ViewButton {
                id: actionButton
                required property var modelData
                objectName: "mediaAction-" + modelData.id
                look: root.look
                text: modelData.label
                enabled: root.currentItem !== null
                onTapped: root.act(actionButton.modelData.id)
            }
        }
    }

    Item {
        id: body
        x: root.u
        y: bar.y + bar.height + root.u
        width: parent.width - 2 * root.u
        height: foot.y - y - (foot.visible ? root.u : 0)

        Text {
            visible: root.items.length === 0
            anchors.fill: parent
            anchors.margins: root.u
            text: root.emptyText
            wrapMode: Text.Wrap
            color: root.look.label
            font.pixelSize: root.look.labelSize
        }

        ListView {
            id: gridList
            objectName: "mediaGrid"
            visible: root.mode === "grid" && root.items.length > 0
            anchors.fill: parent
            clip: true
            model: root.mode === "grid" ? root.gridRows : []
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FugueScrollBar { look: root.look }
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Results")
            delegate: Item {
                id: gridRow
                required property var modelData
                width: gridList.width
                height: modelData.heading !== undefined ? 6 * root.u : root.cell + root.u
                Text {
                    visible: gridRow.modelData.heading !== undefined
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    text: gridRow.modelData.heading ?? ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: root.look.label
                    font.pixelSize: root.look.captionSize
                    font.weight: Font.DemiBold
                }
                Row {
                    spacing: root.u
                    Repeater {
                        model: gridRow.modelData.cells ?? []
                        MediaThumb {
                            id: gridCell
                            required property int modelData
                            objectName: "mediaCell-" + modelData
                            width: root.cell
                            height: root.cell
                            look: root.look
                            item: root.items[modelData]
                            current: modelData === root.currentIndex
                            focused: root.activeFocus
                            onTapped: { root.select(gridCell.modelData); root.forceActiveFocus() }
                            onActivated: { root.select(gridCell.modelData); root.open() }
                        }
                    }
                }
            }
        }

        MediaSingle {
            id: large
            objectName: "mediaLarge"
            visible: root.mode !== "grid"
            x: 0
            width: parent.width
            height: parent.height - (filmstripLoader.active ? root.strip + root.u : 0) - infoBox.reserved
            look: root.look
            item: root.mode === "grid" ? null : root.currentItem
            onPressed: root.forceActiveFocus()
        }
        Loader {
            id: filmstripLoader
            active: root.mode === "filmstrip" && root.items.length > 0
            x: 0
            y: large.height + root.u
            width: parent.width
            height: root.strip
            sourceComponent: ListView {
                objectName: "mediaFilmstrip"
                orientation: ListView.Horizontal
                spacing: root.u
                clip: true
                model: root.items
                boundsBehavior: Flickable.StopAtBounds
                Component.onCompleted: positionViewAtIndex(Math.max(0, root.currentIndex), ListView.Contain)
                ScrollBar.horizontal: FugueScrollBar { look: root.look }
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Filmstrip")
                delegate: MediaThumb {
                    id: stripCell
                    required property var modelData
                    required property int index
                    objectName: "mediaStripCell-" + index
                    width: root.strip - 2 * root.u
                    height: root.strip - 2 * root.u
                    look: root.look
                    item: modelData
                    current: index === root.currentIndex
                    focused: root.activeFocus
                    onTapped: { root.select(stripCell.index); root.forceActiveFocus() }
                    onActivated: { root.select(stripCell.index); root.open() }
                }
            }
        }
        // The details of the current item, under the large view.
        Flickable {
            id: infoBox
            objectName: "mediaDetails"
            readonly property bool wanted: root.showInfo && root.mode !== "grid" && root.currentItem !== null && root.details.length > 0
            readonly property real reserved: wanted ? height + root.u : 0
            visible: wanted
            x: 0
            y: large.height + (filmstripLoader.active ? root.strip + 2 * root.u : root.u)
            width: parent.width
            height: wanted ? Math.min(contentHeight, 24 * root.u) : 0
            contentHeight: infoColumn.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FugueScrollBar { look: root.look }
            Column {
                id: infoColumn
                width: infoBox.width
                spacing: root.u / 2
                Repeater {
                    model: root.details
                    Row {
                        id: infoRow
                        required property var modelData
                        width: infoColumn.width
                        spacing: root.u
                        Text {
                            id: infoLabel
                            width: 16 * root.u
                            text: infoRow.modelData.label
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: root.look.label
                            font.pixelSize: root.look.captionSize
                        }
                        Text {
                            width: infoRow.width - infoLabel.width - infoRow.spacing
                            text: infoRow.modelData.value
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            maximumLineCount: 4
                            elide: Text.ElideRight
                            color: root.look.text
                            font.pixelSize: root.look.captionSize
                        }
                    }
                }
            }
        }
    }
}
