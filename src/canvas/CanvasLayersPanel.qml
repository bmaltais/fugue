pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "widgets"

// The layers of the canvas a Canvas view shows (#226, spec 14.5), as an outliner: compact rows top first, each with a
// thumbnail, a second one for its layer mask, the name and the eye at the right; under the list the selected layer's
// properties (Opacity as a drag-number field, Blend, the layer mask) and the arrange buttons. Simple first: the eye,
// Opacity and Delete are in view at once; blend modes, the mask and the keys are there for who wants them.
//
// Every edit is CanvasViewModel's (canvas/canvas_view_model.h): ONE new revision of the head, which the view then shows;
// Undo and Redo walk the head. Nothing is editable when the view shows another revision than the head: the list then
// reads only and says so.
//
// Pointer: a click selects (Ctrl adds or takes away, Shift a range), a double click on the name or F2 renames, a drag
// moves the selected layers (a line shows where), a right click opens the menu of the same operations with their keys.
// The eye shows or hides (the selection when the row is selected), Alt+click shows that layer alone (solo: a view, never a
// revision; again ends it). A click on the mask's thumbnail makes the view's brush paint that mask (#230, Photoshop's:
// the thumbnail gets an accent frame, and `maskTargeted` tells the view to take the brush); **Add mask** gives the
// selected layer a fully revealing one (one revision) and targets it. Shift+click on the mask turns it off or on. Keys,
// while the pointer is over the panel or it has the focus (the `canvas-layers` context of the command registry,
// rebindable): H, F2, Ctrl+D, ] and [, X or Delete, Ctrl+Z and Ctrl+Shift+Z. `ownShortcuts` binds them here as well, for a host that is a modal popup (the File > Canvas…
// dialog), whose window's shortcuts sleep.
//
// Rows are 8u (32px) with 24px thumbnails and a 28px eye; every control is 28px. A group (#252) is one row of kind
// "group" whose thumbnail is its children's composite; its children are not listed (Ungroup takes them out), and a row
// has `depth`, which indents it, so a nested list can come without a new model.
Rectangle {
    id: root
    objectName: "canvasLayersPanel"
    required property CanvasLook look
    required property CanvasViewModel model
    property bool ownShortcuts: false
    // A layer mask became the brush's target (a click on its thumbnail, Add mask): the view takes the brush.
    signal maskTargeted
    // Layer properties… of this layer (the view opens the dialog).
    signal propertiesRequested(int position)
    readonly property real u: look.unit
    readonly property var rows: model.layers
    readonly property var chosen: model.selection
    readonly property bool editable: model.layersEditable
    // The selected layer whose properties show: the top one of the selection.
    readonly property var current: {
        for (let i = 0; i < rows.length; ++i)
            if (chosen.indexOf(rows[i].position) >= 0)
                return rows[i]
        return null
    }
    readonly property bool hot: hover.hovered || list.activeFocus
    property int dropGap: -1 // the drop line while a row is dragged: above row `dropGap` of the top-first list
    property int renaming: -1 // the position whose name is being typed

    width: 56 * u
    radius: look.smallCorner
    color: look.body // the controls inside are wells, so they read as recessed against it
    border.width: 1
    border.color: look.hairline

    HoverHandler { id: hover }
    onHotChanged: App.commands.setContextActive("canvas-layers", hot)
    Component.onDestruction: if (hot) App.commands.setContextActive("canvas-layers", false)

    // What the selection means for a row's own buttons: the selection when the row is in it, else the row alone.
    function targetsFor(position) { return chosen.indexOf(position) >= 0 ? chosen : [position] }
    function keysOf(id) { return (App.commands.entry(id).keys ?? []).join(", ") }
    function blendLabel(id) {
        const found = model.blendModes.find(b => b.id === id)
        return found ? found.label : (current ? current.compositeOp : "")
    }
    // Whether the layer and the one under it are shown, unlocked paint layers or groups, which Merge down needs.
    function canMergeDown(row) {
        const under = rows.find(r => r.position === row.position - 1)
        const merges = r => r !== undefined && r.visible && !r.locked && r.kind !== "mask" && r.kind !== "picture"
        return merges(row) && merges(under)
    }
    // Whether any of these positions is locked (#226): a delete, a mask delete or a merge of it is refused.
    function lockedIn(positions) { return positions.some(p => rows.find(r => r.position === p)?.locked === true) }
    function addMask(position) {
        if (editable && model.addMask(position))
            maskTargeted()
    }
    function paintMask(position) {
        if (editable && model.targetMask(position))
            maskTargeted()
    }
    function startRename(position) {
        if (editable)
            renaming = position
    }

    // The commands of the registry for this panel; the one the pointer is over (or that has the focus) does them.
    function run(id) {
        const sel = chosen
        switch (id) {
        case "canvas-layers.visible": if (sel.length > 0) model.toggleVisible(sel); break
        case "canvas-layers.lock": if (sel.length > 0) model.toggleLocked(sel); break
        case "canvas-layers.use-in-region": if (current && sel.length === 1) model.setRegionLayer(current.position); break
        case "canvas-layers.rename": if (current) startRename(current.position); break
        case "canvas-layers.duplicate": if (sel.length > 0) model.duplicate(sel); break
        case "canvas-layers.raise": if (sel.length > 0) model.raise(sel); break
        case "canvas-layers.lower": if (sel.length > 0) model.lower(sel); break
        case "canvas-layers.delete": if (sel.length > 0) model.removeLayers(sel); break
        case "canvas-layers.mask": if (current && current.hasMask) model.setMaskEnabled(sel, !current.maskEnabled); break
        case "canvas-layers.undo": model.undo(); break
        case "canvas-layers.redo": model.redo(); break
        }
    }
    Connections {
        target: App.commands
        function onWindowRequested(id) {
            if (root.hot && id.startsWith("canvas-layers."))
                root.run(id)
        }
    }
    // In a modal popup the window's shortcuts sleep: the panel carries the keys of its own commands.
    Instantiator {
        model: root.ownShortcuts ? App.commands.activeKeys.filter(k => App.commands.ownerOf(k).startsWith("canvas-layers.")) : []
        delegate: Shortcut {
            required property string modelData
            sequence: modelData
            enabled: root.hot && !(App.commands.textFieldFocus && App.commands.belongsToTextField(modelData))
            onActivated: App.commands.triggerKey(modelData)
        }
    }

    Component {
        id: menuComponent
        WidgetMenu {
            id: menu
            look: root.look
            parent: Overlay.overlay
            onClosed: menu.destroy()
        }
    }
    function openMenu(item, x, y, entries, picked) {
        const at = item.mapToItem(Overlay.overlay, x, y)
        const menu = menuComponent.createObject(root, {wantX: at.x, wantY: at.y, width: 64 * u, anchorHeight: 0, entries: entries}) as WidgetMenu
        menu.picked.connect(picked)
        menu.open()
    }
    // The right-click menu: the same operations as the keys, with the keys shown. No delete is the last row.
    function openRowMenu(item, x, y, row) {
        const sel = targetsFor(row.position)
        const off = !editable
        const entries = [
            {text: row.visible ? qsTr("Hide") : qsTr("Show"), value: "visible", detail: keysOf("canvas-layers.visible"), enabled: !off},
            {text: model.solo === row.position ? qsTr("Show every layer") : qsTr("Show this layer alone"), value: "solo", detail: qsTr("Alt+click the eye")},
            {text: row.locked ? qsTr("Unlock") : qsTr("Lock"), value: "lock", detail: keysOf("canvas-layers.lock"), enabled: !off},
            {text: model.regionLayer === row.name && row.kind === "paint" ? qsTr("Generate reads the whole picture") : qsTr("Generate reads this layer only"), value: "region", detail: keysOf("canvas-layers.use-in-region"),
             enabled: !off && sel.length === 1 && row.kind === "paint"},
            {text: qsTr("Rename…"), value: "rename", detail: keysOf("canvas-layers.rename"), enabled: !off && sel.length === 1},
            {heading: true, enabled: false, text: qsTr("Remove")},
            {text: sel.length > 1 ? qsTr("Delete %n layers", "", sel.length) : qsTr("Delete"), value: "delete", detail: keysOf("canvas-layers.delete"), enabled: !off && !lockedIn(sel)},
            {heading: true, enabled: false, text: qsTr("Arrange")},
            {text: qsTr("Duplicate"), value: "duplicate", detail: keysOf("canvas-layers.duplicate"), enabled: !off},
            {text: qsTr("Move up"), value: "raise", detail: keysOf("canvas-layers.raise"), enabled: !off},
            {text: qsTr("Move down"), value: "lower", detail: keysOf("canvas-layers.lower"), enabled: !off},
            {heading: true, enabled: false, text: qsTr("Combine")},
            {text: qsTr("New layer"), value: "new", detail: keysOf("canvas-view.new-layer"), enabled: !off},
            {text: qsTr("Group layers"), value: "group", detail: keysOf("canvas-view.group"), enabled: !off && row.kind !== "picture"},
            {text: qsTr("Ungroup layers"), value: "ungroup", detail: keysOf("canvas-view.ungroup"), enabled: !off && row.kind === "group"},
            {text: qsTr("Merge down"), value: "merge", detail: keysOf("canvas-view.merge-down"), enabled: !off && sel.length === 1 && canMergeDown(row)},
            {text: qsTr("Layer properties…"), value: "properties", enabled: !off && sel.length === 1}
        ]
        if (row.hasMask)
            entries.push({heading: true, enabled: false, text: qsTr("Layer mask")},
                         {text: qsTr("Delete the mask"), value: "mask-delete", enabled: !off && !lockedIn(sel)},
                         {text: qsTr("Paint the mask"), value: "mask-paint", detail: qsTr("Click the mask"), enabled: !off},
                         {text: row.maskEnabled ? qsTr("Turn the mask off") : qsTr("Turn the mask on"), value: "mask", detail: qsTr("Shift+click the mask"), enabled: !off})
        else if (row.kind !== "mask")
            entries.push({heading: true, enabled: false, text: qsTr("Layer mask")},
                         {text: qsTr("Add mask"), value: "mask-add", detail: keysOf("canvas-view.add-mask"), enabled: !off && sel.length === 1})
        openMenu(item, x, y, entries, value => {
            switch (value) {
            case "visible": model.toggleVisible(sel); break
            case "solo": model.solo = row.position; break
            case "lock": model.toggleLocked(sel); break
            case "region": model.setRegionLayer(row.position); break
            case "rename": startRename(row.position); break
            case "delete": model.removeLayers(sel); break
            case "duplicate": model.duplicate(sel); break
            case "raise": model.raise(sel); break
            case "lower": model.lower(sel); break
            case "new": model.newLayer(); break
            case "group": model.groupLayers(sel); break
            case "ungroup": model.ungroup(sel); break
            case "merge": model.mergeDown(row.position); break
            case "properties": root.propertiesRequested(row.position); break
            case "mask": model.setMaskEnabled(sel, !row.maskEnabled); break
            case "mask-delete": model.removeMask(sel); break
            case "mask-paint": root.paintMask(row.position); break
            case "mask-add": root.addMask(row.position); break
            }
        })
    }

    Column {
        id: column
        anchors.fill: parent
        anchors.margins: root.u
        spacing: root.u

        // The caption, and Undo and Redo of the canvas (the head back and forward).
        Item {
            width: parent.width
            height: 7 * root.u
            Text {
                anchors.verticalCenter: parent.verticalCenter
                leftPadding: root.u
                text: qsTr("Layers")
                color: root.look.label
                font.pixelSize: root.look.captionSize
            }
            Row {
                anchors.right: parent.right
                spacing: root.u
                ViewButton {
                    objectName: "canvasLayersUndo"
                    look: root.look
                    glyph: "undo-2"
                    accented: false
                    enabled: root.model.canUndo
                    tip: qsTr("Undo the last canvas change (%1)").arg(root.keysOf("canvas-layers.undo"))
                    onTapped: root.model.undo()
                }
                ViewButton {
                    objectName: "canvasLayersRedo"
                    look: root.look
                    glyph: "redo-2"
                    accented: false
                    enabled: root.model.canRedo
                    tip: qsTr("Redo the canvas change (%1)").arg(root.keysOf("canvas-layers.redo"))
                    onTapped: root.model.redo()
                }
            }
        }

        ListView {
            id: list
            objectName: "canvasLayerList"
            width: parent.width
            height: Math.max(8 * root.u, parent.height - y - properties.height - parent.spacing)
            clip: true
            model: root.rows
            spacing: root.u / 2
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: true
            ScrollBar.vertical: FugueScrollBar { look: root.look }
            Keys.onPressed: event => {
                const at = root.current ? root.rows.findIndex(r => r.position === root.current.position) : -1
                if (event.key === Qt.Key_Up && at > 0) root.model.selectLayer(root.rows[at - 1].position, event.modifiers & Qt.ShiftModifier ? "range" : "only")
                else if (event.key === Qt.Key_Down && at >= 0 && at < root.rows.length - 1) root.model.selectLayer(root.rows[at + 1].position, event.modifiers & Qt.ShiftModifier ? "range" : "only")
                else return
                event.accepted = true
            }

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property int position: modelData.position
                readonly property bool selected: root.chosen.indexOf(position) >= 0
                objectName: "canvasLayer-" + position
                width: list.width
                height: 8 * root.u
                radius: root.look.smallCorner
                color: selected ? root.look.selectedRow : rowHover.hovered ? root.look.menuRow : "transparent"
                border.width: root.current !== null && root.current.position === position && list.activeFocus ? 2 : 0
                border.color: root.look.accent
                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.name
                Accessible.selected: selected
                opacity: drag.active ? 0.6 : 1

                HoverHandler { id: rowHover }
                // A press on the eye is the eye's, not a selection; so is a press on the mask (a click targets it, #230;
                // Shift+click turns it off), whatever the modifiers.
                function onToggle(point: eventPoint, shifted: bool): bool {
                    const onEye = eye.contains(eye.mapFromItem(row, point.position))
                    const onMask = maskThumb.visible && maskThumb.contains(maskThumb.mapFromItem(row, point.position))
                    return onEye || onMask
                }
                ToolTip.visible: rowHover.hovered && modelData.from !== "" && !drag.active && root.renaming !== position
                ToolTip.delay: root.look.tooltipDelay
                ToolTip.text: modelData.created !== "" ? qsTr("From %1, %2").arg(modelData.from).arg(modelData.created) : qsTr("From %1").arg(modelData.from)

                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    acceptedModifiers: Qt.NoModifier
                    onTapped: point => {
                        if (row.onToggle(point, false))
                            return
                        root.model.selectLayer(row.position, "only")
                        list.forceActiveFocus()
                    }
                    onDoubleTapped: point => { if (!row.onToggle(point, false)) root.startRename(row.position) }
                }
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    acceptedModifiers: Qt.ControlModifier
                    onTapped: point => { if (!row.onToggle(point, false)) { root.model.selectLayer(row.position, "toggle"); list.forceActiveFocus() } }
                }
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    acceptedModifiers: Qt.ShiftModifier
                    onTapped: point => { if (!row.onToggle(point, true)) { root.model.selectLayer(row.position, "range"); list.forceActiveFocus() } }
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: point => {
                        if (!row.selected)
                            root.model.selectLayer(row.position)
                        root.openRowMenu(row, point.position.x, point.position.y, row.modelData)
                    }
                }
                // Drag to reorder: the selection when this row is in it; the line shows the gap it lands in.
                DragHandler {
                    id: drag
                    target: null
                    enabled: root.editable && root.renaming < 0
                    yAxis.enabled: true
                    xAxis.enabled: false
                    onActiveChanged: {
                        if (active) {
                            if (!row.selected)
                                root.model.selectLayer(row.position)
                            return
                        }
                        const gap = root.dropGap
                        root.dropGap = -1
                        if (gap >= 0)
                            root.model.dropAt(root.chosen, gap)
                    }
                    onCentroidChanged: {
                        if (!active)
                            return
                        const y = row.mapToItem(list.contentItem, 0, centroid.position.y).y
                        const step = row.height + list.spacing
                        root.dropGap = Math.max(0, Math.min(root.rows.length, Math.round(y / step)))
                    }
                }

                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: root.u + row.modelData.depth * 4 * root.u
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: root.u
                    // The layer's own pixels on a checkerboard (its transparency shows).
                    Rectangle {
                        objectName: "canvasLayerThumb-" + row.position
                        width: 6 * root.u
                        height: 6 * root.u
                        color: "#808080"
                        border.width: 1
                        border.color: root.look.hairline
                        Grid {
                            anchors.fill: parent
                            anchors.margins: 1
                            columns: 4
                            Repeater {
                                model: 16
                                delegate: Rectangle {
                                    required property int index
                                    width: (6 * root.u - 2) / 4
                                    height: width
                                    color: (index + Math.floor(index / 4)) % 2 === 0 ? "#9a9a9a" : "#6a6a6a"
                                }
                            }
                        }
                        Image {
                            anchors.fill: parent
                            anchors.margins: 1
                            source: row.modelData.thumbnail
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            smooth: true
                        }
                    }
                    // The layer mask: a click makes the brush paint it (an accent frame says so, #230), Shift+click turns it
                    // off or on; off, it is crossed and dim.
                    Rectangle {
                        id: maskThumb
                        objectName: "canvasLayerMask-" + row.position
                        readonly property bool targeted: root.model.maskTarget === row.position
                        visible: row.modelData.hasMask
                        width: 6 * root.u
                        height: 6 * root.u
                        color: "black"
                        border.width: targeted ? 2 : 1
                        border.color: targeted ? root.look.accent : root.look.hairline
                        opacity: row.modelData.maskEnabled ? 1 : 0.45
                        Image {
                            anchors.fill: parent
                            anchors.margins: maskThumb.border.width
                            source: row.modelData.maskThumbnail
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        Glyph {
                            anchors.centerIn: parent
                            visible: !row.modelData.maskEnabled
                            look: root.look
                            name: "x"
                            color: root.look.error
                        }
                        TapHandler {
                            acceptedModifiers: Qt.ShiftModifier
                            onTapped: if (root.editable) root.model.setMaskEnabled([row.position], !row.modelData.maskEnabled)
                        }
                        TapHandler {
                            acceptedModifiers: Qt.NoModifier
                            onTapped: root.paintMask(row.position)
                        }
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Layer mask of %1").arg(row.modelData.name)
                        Accessible.checked: targeted
                        ToolTip.visible: maskHover.hovered
                        ToolTip.delay: root.look.tooltipDelay
                        ToolTip.text: maskThumb.targeted ? qsTr("The brush paints this mask: black hides the layer, white shows it. Shift+click turns it off.")
                                      : row.modelData.maskEnabled ? qsTr("Layer mask: white shows the layer, black hides it. Click to paint it, Shift+click turns it off.")
                                      : qsTr("Layer mask, off: the layer shows whole. Click to paint it, Shift+click turns it on.")
                        HoverHandler { id: maskHover }
                    }
                }
                Text {
                    id: nameText
                    objectName: "canvasLayerName-" + row.position
                    anchors.verticalCenter: parent.verticalCenter
                    x: (row.modelData.hasMask ? 16 : 9) * root.u + row.modelData.depth * 4 * root.u
                    width: lockToggle.x - x - root.u
                    visible: root.renaming !== row.position
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: row.modelData.name
                    color: root.look.text
                    opacity: row.modelData.visible ? 1 : 0.55
                    font.pixelSize: root.look.labelSize
                }
                ThemedTextField {
                    objectName: "canvasLayerRename"
                    visible: root.renaming === row.position
                    x: nameText.x
                    width: nameText.width
                    height: 6 * root.u
                    anchors.verticalCenter: parent.verticalCenter
                    padding: 2
                    font.pixelSize: root.look.labelSize
                    maximumLength: 200
                    onVisibleChanged: if (visible) { text = row.modelData.name; forceActiveFocus(); selectAll() }
                    onAccepted: {
                        const name = text.trim()
                        root.renaming = -1
                        if (name !== "" && name !== row.modelData.name)
                            root.model.rename(row.position, name)
                        list.forceActiveFocus()
                    }
                    Keys.onEscapePressed: event => { root.renaming = -1; list.forceActiveFocus(); event.accepted = true }
                    onActiveFocusChanged: if (!activeFocus && root.renaming === row.position) root.renaming = -1
                }
                // The lock, left of the eye as in Blender's outliner: quiet and dim while open, lit while locked. The font has no
                // padlock yet, so the pin stands for it.
                ViewButton {
                    id: lockToggle
                    objectName: "canvasLayerLock-" + row.position
                    anchors.right: eye.left
                    anchors.rightMargin: root.u / 2
                    anchors.verticalCenter: parent.verticalCenter
                    look: root.look
                    glyph: "pin"
                    accented: false
                    checkable: true
                    current: row.modelData.locked === true
                    opacity: row.modelData.locked === true ? 1 : 0.4
                    tip: row.modelData.locked === true ? qsTr("Locked: it cannot be deleted, merged, transformed or have its mask changed. Click to unlock (%1)").arg(root.keysOf("canvas-layers.lock"))
                                                       : qsTr("Lock the layer so it cannot be deleted, merged, transformed or have its mask changed (%1)").arg(root.keysOf("canvas-layers.lock"))
                    onTapped: if (root.editable) root.model.setLocked(root.targetsFor(row.position), row.modelData.locked !== true)
                }
                // The eye, at the right as an outliner's toggles are. A hidden layer's is flat and dim (the font has no
                // crossed eye). While one layer is shown alone its eye alone is on and the others' are flat and dim, as
                // the picture is: the quiet marking, never the accent fill.
                ViewButton {
                    id: eye
                    objectName: "canvasLayerEye-" + row.position
                    readonly property bool shown: root.model.solo >= 0 ? root.model.solo === row.position : row.modelData.visible
                    anchors.right: parent.right
                    anchors.rightMargin: root.u / 2
                    anchors.verticalCenter: parent.verticalCenter
                    look: root.look
                    glyph: "eye"
                    accented: false
                    checkable: true
                    current: eye.shown
                    opacity: eye.shown ? 1 : 0.55
                    tip: root.model.solo === row.position ? qsTr("Shown alone; Alt+click shows every layer again")
                         : row.modelData.visible ? qsTr("Hide the layer (H); Alt+click shows it alone") : qsTr("Show the layer (H); Alt+click shows it alone")
                    altAction: true
                    onTapped: if (root.editable) root.model.setVisible(root.targetsFor(row.position), !row.modelData.visible)
                    onAltTapped: root.model.solo = row.position
                }
            }

            // The drop line while a row is dragged.
            Rectangle {
                objectName: "canvasLayerDropLine"
                visible: root.dropGap >= 0
                x: 0
                width: list.width
                height: 2
                color: root.look.accent
                y: root.dropGap * (8 * root.u + list.spacing) - list.contentY - 1
                z: 3
            }
        }

        // The selected layer's properties, then arrange and delete; or why nothing can be changed.
        Column {
            id: properties
            width: parent.width
            spacing: root.u

            // Read only (another step than the latest is shown): why, in plain words, and what to do about it here.
            Column {
                objectName: "canvasLayersReadOnly"
                visible: !root.editable && root.rows.length > 0
                width: parent.width
                spacing: root.u
                Text {
                    objectName: "canvasLayersReadOnlyText"
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: root.look.label
                    font.pixelSize: root.look.captionSize
                    text: qsTr("You are looking at an earlier step. Go back to the latest version to change layers.")
                }
                Flow {
                    width: parent.width
                    spacing: root.u
                    ViewButton {
                        objectName: "canvasLayersGoHead"
                        look: root.look
                        text: qsTr("Back to latest")
                        tip: qsTr("Show the latest version, whose layers can be changed")
                        onTapped: root.model.show(root.model.head)
                    }
                }
            }
            // A notice with its action, in the window's one notice look (Banner.qml), not an accent tint.
            Banner {
                objectName: "canvasLayersSolo"
                visible: root.model.solo >= 0
                width: parent.width
                look: root.look
                Item {
                    width: parent.width
                    height: soloEnd.height
                    Text {
                        anchors.left: parent.left
                        anchors.right: soloEnd.left
                        anchors.rightMargin: root.u
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideRight
                        color: root.look.text
                        font.pixelSize: root.look.captionSize
                        text: qsTr("Showing %1 alone").arg(root.rows.find(r => r.position === root.model.solo)?.name ?? "")
                    }
                    ViewButton {
                        id: soloEnd
                        objectName: "canvasLayersSoloEnd"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        look: root.look
                        glyph: "x"
                        flat: true
                        compact: true
                        tip: qsTr("Show every layer")
                        onTapped: root.model.solo = -1
                    }
                }
            }
            // Which layer Generate reads (#226): a view state like Show alone, so a notice with its way back.
            Banner {
                objectName: "canvasLayersRegion"
                visible: root.model.regionLayer !== ""
                width: parent.width
                look: root.look
                Item {
                    width: parent.width
                    height: regionEnd.height
                    Text {
                        anchors.left: parent.left
                        anchors.right: regionEnd.left
                        anchors.rightMargin: root.u
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideRight
                        color: root.look.text
                        font.pixelSize: root.look.captionSize
                        text: qsTr("Generate reads %1 only").arg(root.model.regionLayer)
                    }
                    ViewButton {
                        id: regionEnd
                        objectName: "canvasLayersRegionEnd"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        look: root.look
                        glyph: "x"
                        flat: true
                        compact: true
                        tip: qsTr("Generate reads the whole picture again")
                        onTapped: root.model.setRegionLayer(-1)
                    }
                }
            }
            DragNumberField {
                objectName: "canvasLayerOpacity"
                width: parent.width
                look: root.look
                label: qsTr("Opacity")
                tip: qsTr("Drag to change, click to type, double-click for 100%; Ctrl snaps to 10%, Shift is finer")
                enabled: root.editable && root.current !== null
                value: root.current ? root.current.opacity : 1
                from: 0
                to: 1
                step: 0.01
                snap: 0.1
                defaultValue: 1
                format: v => qsTr("%1%").arg(Math.round(v * 100))
                parse: text => Number(String(text).replace("%", "").trim()) / 100
                onCommitted: v => root.model.setOpacity(root.chosen, v)
            }
            ViewButton {
                objectName: "canvasLayerBlend"
                width: parent.width
                look: root.look
                accented: false
                enabled: root.editable && root.current !== null
                text: qsTr("Blend: %1").arg(root.current ? root.blendLabel(root.current.blend) : qsTr("Normal"))
                trailingGlyph: "chevron-down"
                tip: qsTr("How the layer mixes with the layers under it")
                onTapped: {
                    const now = root.current ? root.current.blend : "normal"
                    root.openMenu(this, 0, height, root.model.blendModes.map(b => ({text: b.label, value: b.id, checked: b.id === now})),
                                  value => root.model.setBlend(root.chosen, value))
                }
            }
            // A layer with no mask: Add mask (#230), which the brush then paints.
            ViewButton {
                objectName: "canvasLayerAddMask"
                visible: root.current !== null && root.current.hasMask !== true && root.current.kind !== "mask"
                look: root.look
                accented: false
                enabled: root.editable && root.chosen.length === 1
                text: qsTr("Add mask")
                tip: qsTr("Give the layer a mask that shows it all, and paint it to hide parts (%1)").arg(root.keysOf("canvas-view.add-mask"))
                onTapped: root.addMask(root.current.position)
            }
            // The layer mask of the selected layer, when it has one.
            Row {
                visible: root.current !== null && root.current.hasMask === true
                spacing: root.u
                ViewButton {
                    objectName: "canvasLayerMaskToggle"
                    look: root.look
                    accented: false
                    enabled: root.editable
                    text: root.current && root.current.maskEnabled ? qsTr("Mask on") : qsTr("Mask off")
                    checkable: true
                    current: root.current !== null && root.current.maskEnabled === true
                    tip: qsTr("Turn the layer mask off to see the whole layer, or on again (Shift+click the mask)")
                    onTapped: root.model.setMaskEnabled(root.chosen, !(root.current && root.current.maskEnabled))
                }
                Item { width: root.u; height: 1 }
                ViewButton {
                    objectName: "canvasLayerMaskDelete"
                    look: root.look
                    danger: true
                    enabled: root.editable && !root.lockedIn(root.chosen)
                    text: qsTr("Delete mask")
                    tip: qsTr("Delete the layer mask (Undo brings it back)")
                    onTapped: root.model.removeMask(root.chosen)
                }
            }
            Text {
                objectName: "canvasLayerInfo"
                visible: root.current !== null && root.current.from !== ""
                width: parent.width
                elide: Text.ElideRight
                color: root.look.label
                font.pixelSize: root.look.captionSize
                text: root.current ? (root.current.created !== "" ? qsTr("From %1, %2").arg(root.current.from).arg(root.current.created.replace("T", " ").slice(0, 16))
                                                                  : qsTr("From %1").arg(root.current.from)) : ""
            }
            Row {
                spacing: root.u
                ViewButton {
                    objectName: "canvasLayerDuplicate"
                    look: root.look
                    accented: false
                    enabled: root.editable && root.chosen.length > 0
                    text: qsTr("Duplicate")
                    tip: qsTr("Duplicate the selected layers (%1)").arg(root.keysOf("canvas-layers.duplicate"))
                    onTapped: root.model.duplicate(root.chosen)
                }
                ViewButton {
                    objectName: "canvasLayerUp"
                    look: root.look
                    glyph: "arrow-up"
                    accented: false
                    enabled: root.editable && root.chosen.length > 0
                    tip: qsTr("Move the selected layers up (%1)").arg(root.keysOf("canvas-layers.raise"))
                    onTapped: root.model.raise(root.chosen)
                }
                ViewButton {
                    objectName: "canvasLayerDown"
                    look: root.look
                    glyph: "arrow-down"
                    accented: false
                    enabled: root.editable && root.chosen.length > 0
                    tip: qsTr("Move the selected layers down (%1)").arg(root.keysOf("canvas-layers.lower"))
                    onTapped: root.model.lower(root.chosen)
                }
                // The destructive one, apart by a gap twice the others; Undo brings it back.
                Item { width: root.u; height: 1 }
                ViewButton {
                    objectName: "canvasLayerDelete"
                    look: root.look
                    glyph: "trash-2"
                    danger: true
                    enabled: root.editable && root.chosen.length > 0 && !root.lockedIn(root.chosen)
                    tip: qsTr("Delete the selected layers (%1); Undo brings them back").arg(root.keysOf("canvas-layers.delete"))
                    onTapped: root.model.removeLayers(root.chosen)
                }
            }
        }
    }
}
