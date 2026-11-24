pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import ".."
import "../canvasMenu.js" as CanvasMenu

// One value input on a node's face, as a widget (spec 6.2, 3.2, 2.5; canvas-look 4.4): a
// recessed well across the row with the value at its right; the row itself (CanvasNode.qml)
// draws the label over it. A linked input shows where its value comes from instead.
// Everything that decides (constraints, parsing, checks, the undo step) is WidgetEdit's
// (canvas/widget_edit.h); this file draws and forwards.
//
// At rest a widget is the well, one text, a glyph and a mouse area, so a big graph stays cheap
// (12.1). A slider's fill, a toggle's switch and a color's swatch are made only for those widgets,
// spin chevrons on the first hover, and what a widget needs only while it is used (the text
// editor, a menu, a file or color picker) on the first click, dropped after. Typed fields are
// QQuickTextInput or QQuickTextEdit, so while one has focus the keys of 12.4 belong to it.
Rectangle {
    id: root
    objectName: "inputWidget"

    required property WidgetEdit edits // CanvasModel.widgets
    required property string nodeId
    required property var port  // CanvasModel::portMap
    required property CanvasLook look  // App.theme.look (look.h)
    property var canvasView     // the FugueCanvas: takes the focus back when an edit ends, opens the mask editor
    property Item nodeItem      // its rows, for Tab
    property Item row           // raised while the multiline editor covers the rows below

    readonly property string name: port.name
    readonly property string kind: port.widgetType !== undefined ? port.widgetType : ""
    readonly property bool linked: port.wired
    readonly property bool editable: kind !== "" && !linked
    // Fields typed into; Tab moves between them.
    readonly property bool typed: ["spin", "slider", "seed", "line", "multiline", "file", "folder", "color"]
                                  .indexOf(kind) >= 0
    readonly property var literal: port.literal
    readonly property bool isDefault: port.isDefault === true
    property Item editor: null
    readonly property bool editingText: editor !== null
    // Why the text being typed is not a value; shown under the field.
    property string error: ""

    // docs/canvas-look.md 4.4: a recessed well, small corner, 2u side padding.
    readonly property color wellColor: look.well
    readonly property int smallCorner: look.smallCorner
    readonly property int pad: Math.round(2 * look.unit)
    // A multiline value fills its tall row: up to three lines from the top left.
    readonly property bool tall: kind === "multiline" && !linked
    readonly property int glyphZone: Math.round(look.textSize * 1.5)
    // The room a toggle's switch or a color's swatch takes at the right end.
    // A Load Image's file has a Paste button beside its picker (workflow/image_inputs.h).
    readonly property bool pasteable: editable && kind === "file" && edits.canPasteImage(nodeId, name)
    readonly property int pasteWidth: Math.round(look.textSize * 3.6)
    readonly property int decorWidth: !editable ? 0 : kind === "toggle" ? 26 : kind === "color" ? glyphZone
                                      : pasteable ? pasteWidth + pad : 0
    // A toggle's state, the default when unset.
    readonly property bool on: kind === "toggle"
        && (literal !== undefined ? literal === true : edits.spec(nodeId, name)["default"] === true)

    // The number shown, or the default when unset.
    readonly property real number: typeof literal === "number" ? literal
                                   : port.value !== undefined && port.value !== "" && !isNaN(Number(port.value))
                                     ? Number(port.value) : 0
    // A model file picked and still being hashed (WidgetEdit::setDiskModel): {progress, name},
    // {error} when it could not be read, or empty.
    readonly property var hashing: kind === "model" && edits.hashRevision >= 0 ? edits.hashState(nodeId, name) : ({})
    // A canvas input (#211): the id stored, the canvas's row in this root's list (empty when this
    // root has none by that id: it was made in another root, or is gone), re-read when the list changes.
    readonly property string canvasId: kind === "canvas" && port.value !== undefined ? String(port.value) : ""
    readonly property var canvasRow: kind === "canvas" && canvasId !== "" && edits.canvasesRevision >= 0
                                     ? edits.canvasRow(canvasId) : ({})
    readonly property bool canvasMissing: kind === "canvas" && canvasId !== "" && !linked && canvasRow.id === undefined
    readonly property string valueText: {
        if (linked)
            return edits.source(nodeId, name)
        if (kind === "canvas" && canvasId === "")
            return qsTr("Active canvas")
        if (kind === "canvas")
            return canvasRow.id !== undefined ? qsTr("%1 · pinned").arg(canvasRow.name) : qsTr("%1 (missing)").arg(canvasId)
        if (hashing.error !== undefined)
            return hashing.error
        if (hashing.progress !== undefined)
            return qsTr("Reading model… %1%").arg(Math.floor(hashing.progress * 100))
        if (kind === "model" && (port.value === undefined || port.value === ""))
            return port.required ? qsTr("Choose a model") : qsTr("Any ready model")
        return port.value !== undefined ? port.value : ""
    }
    // The glyph after the value (a Lucide name): what a click at the right end opens.
    readonly property string glyph: !editable ? ""
        : kind === "choice" || kind === "model" || kind === "canvas" ? "chevron-down"
        : kind === "seed" ? "dices"
        : kind === "folder" ? "folder"
        : kind === "file" ? "file"
        : kind === "mask" || kind === "paint" ? "brush" : ""
    // A 6.2 color (#rrggbb, #rrggbbaa) as QML reads one (#aarrggbb), or `otherwise`.
    function colorOf(text, otherwise) {
        if (!/^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/.test(text))
            return otherwise
        return text.length === 9 ? "#" + text.substr(7, 2) + text.substr(1, 6) : text
    }
    // The text an editor starts from: the whole stored value, not the face's first line.
    function startText() {
        if (kind === "model")
            return literal !== undefined && literal.name !== undefined ? literal.name : ""
        if (typeof literal === "number")
            return edits.numberText(literal)
        if (typeof literal === "string")
            return literal
        if (literal === undefined && port.value !== undefined && port.placeholder !== true)
            return port.value
        return ""
    }

    radius: smallCorner
    color: editable ? wellColor : "transparent"

    Text {
        id: valueLabel
        objectName: "widgetValue"
        visible: !root.editingText
        x: root.tall ? root.pad : root.width * 0.45
        y: root.tall ? Math.round(1.5 * root.look.unit) + 16 : 0 // under the caption line (#74)
        z: 1
        width: root.width - x - root.pad - root.decorWidth - (glyphMark.visible ? glyphMark.width + root.look.unit : 0)
        // Under the caption line, 1.5u above the bottom (as above the caption).
        height: root.tall ? root.height - y - Math.round(1.5 * root.look.unit) : root.height
        verticalAlignment: root.tall ? Text.AlignTop : Text.AlignVCenter
        horizontalAlignment: root.tall ? Text.AlignLeft : Text.AlignRight
        text: root.valueText
        wrapMode: root.tall ? Text.Wrap : Text.NoWrap
        maximumLineCount: root.tall ? Math.max(1, Math.floor(height / 16)) : 1
        lineHeightMode: root.tall ? Text.FixedHeight : Text.ProportionalHeight
        lineHeight: root.tall ? 16 : 1
        elide: root.kind === "file" || root.kind === "folder" ? Text.ElideMiddle : Text.ElideRight
        textFormat: Text.PlainText
        color: root.hashing.error !== undefined || root.canvasMissing ? root.look.error
               : root.linked || root.isDefault || root.kind === "model" && root.literal === undefined
                 || root.kind === "canvas" && root.canvasId === ""
               ? root.look.label : root.look.text
        font.pixelSize: root.look.labelSize
        font.weight: root.tall ? Font.Normal : Font.Medium
        font.italic: root.linked
        font.features: { "tnum": 1 }
    }
    Glyph {
        id: glyphMark
        visible: root.glyph !== "" && !root.editingText
        x: root.width - root.pad - root.decorWidth - width
        anchors.verticalCenter: parent.verticalCenter
        z: 1
        name: root.glyph
        look: root.look
        color: root.look.label
        font.pixelSize: 12
    }

    // Made only for a Load Image's file input.
    Loader {
        id: pasteLoader
        active: root.pasteable
        z: 2
        sourceComponent: Rectangle {
            objectName: "pasteImageButton"
            visible: !root.editingText
            x: root.width - root.pad - width
            y: 2
            width: root.pasteWidth
            height: root.height - 4
            radius: root.smallCorner
            color: pasteArea.containsMouse ? root.look.selectedRow : "transparent"
            border.color: root.look.hairline
            Text {
                anchors.centerIn: parent
                text: qsTr("Paste")
                color: root.look.label
                font.pixelSize: root.look.labelSize
            }
            MouseArea {
                id: pasteArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.edits.pasteImage(root.nodeId)
            }
        }
    }

    // Made once, only for the widgets that have one.
    function make(file, properties) {
        return Qt.createComponent(file).createObject(root, Object.assign({ host: root }, properties || {}))
    }
    Component.onCompleted: {
        // A slider keeps its fill while linked (hidden), so it shows again when unlinked.
        if (kind === "slider")
            make("SliderFill.qml")
        if (!editable)
            return
        if (kind === "toggle")
            make("ToggleSwitch.qml")
        else if (kind === "color")
            make("ColorSwatch.qml")
    }
    property bool chevronsMade: false

    MouseArea {
        id: mouse
        objectName: "widgetMouse"
        anchors.fill: parent
        enabled: root.editable && !root.editingText
        hoverEnabled: root.kind === "spin"
        preventStealing: true
        cursorShape: root.kind === "spin" || root.kind === "slider" ? Qt.SizeHorCursor
                   : root.typed ? Qt.IBeamCursor : Qt.PointingHandCursor
        property real pressX: 0
        property real startNumber: 0
        property bool dragging: false

        onContainsMouseChanged: {
            if (containsMouse && !root.chevronsMade) {
                root.chevronsMade = true
                root.make("SpinChevrons.qml", { mouse: mouse })
            }
        }
        onPressed: event => {
            // Any edit in progress ends first, as a press on the canvas would end it.
            root.giveFocusBack()
            pressX = event.x
            startNumber = root.number
            dragging = false
        }
        onPositionChanged: event => {
            if (!pressed || (root.kind !== "spin" && root.kind !== "slider"))
                return
            if (!dragging && Math.abs(event.x - pressX) < Application.styleHints.startDragDistance)
                return
            if (!dragging) {
                dragging = true
                root.edits.begin(root.nodeId, root.name)
            }
            if (root.kind === "spin") // scrub: a step per 6 units
                root.edits.previewValue(root.edits.stepped(root.nodeId, root.name, startNumber,
                                                           Math.round((event.x - pressX) / 6)))
            else
                root.edits.previewValue(root.edits.valueAt(root.nodeId, root.name, event.x / root.width))
        }
        onReleased: event => {
            if (dragging) {
                dragging = false
                root.edits.finish()
                return
            }
            root.clicked(event.x)
        }
        onCanceled: {
            if (dragging)
                root.edits.finish()
            dragging = false
        }
    }

    function clicked(x) {
        const buttons = pasteable ? pasteWidth + pad : 0
        const atEnd = x > width - glyphZone - buttons && x <= width - buttons
        switch (kind) {
        case "spin":
            if (x < glyphZone)
                return edits.setValue(nodeId, name, edits.stepped(nodeId, name, number, -1))
            if (atEnd)
                return edits.setValue(nodeId, name, edits.stepped(nodeId, name, number, 1))
            return openEditor()
        case "toggle":
            return edits.setValue(nodeId, name, !on)
        case "mask": // the whole row is the Paint mask button (spec 14.4): the editor opens on the node's picture
            return canvasView ? canvasView.openMask(nodeId) : undefined
        case "paint": // likewise (#181): the paint window, on the node's picture or a blank canvas
            return canvasView ? canvasView.openPaint(nodeId) : undefined
        case "choice":
            return openMenu(choiceEntries())
        case "canvas":
            return openMenu(canvasEntries(), "canvases")
        case "seed":
            return atEnd ? openMenu(seedEntries()) : openEditor()
        case "model":
            edits.requestModels(nodeId, name)
            return openMenu(null)
        case "file":
        case "folder":
        case "color":
            if (atEnd && pasteable)
                return openMenu(inputEntries())
            return atEnd ? openDialog() : openEditor()
        default:
            return openEditor()
        }
    }

    // --- typing ---

    function openEditor() {
        if (editor)
            return
        error = ""
        // The multiline editor opens over the rows below: lift this row above them.
        if (row)
            row.z = 2
        editor = make(kind === "multiline" ? "AreaEditor.qml" : "FieldEditor.qml")
    }
    function closeEditor() {
        error = ""
        const closing = editor
        editor = null
        if (row)
            row.z = 0
        if (closing)
            closing.destroy()
    }
    // Tab and Shift+Tab: the next or previous typed field of this node.
    function focusNeighbour(step) {
        if (!nodeItem)
            return false
        const fields = []
        for (const r of nodeItem.children)
            for (const c of r.children)
                if (c.objectName === "widgetLoader" && c.item && c.item.typed && c.item.editable)
                    fields.push(c.item)
        fields.sort((a, b) => a.port.y - b.port.y)
        const at = fields.indexOf(root)
        if (fields.length < 2 || at < 0)
            return false
        fields[(at + step + fields.length) % fields.length].openEditor()
        return true
    }
    // The focus goes back to the canvas. A field still holding it gives it up first: the
    // canvas is its focus scope, so forceActiveFocus alone would hand focus back to the field.
    function giveFocusBack() {
        if (Window.activeFocusItem)
            Window.activeFocusItem.focus = false
        if (canvasView)
            canvasView.forceActiveFocus()
    }

    // --- menus ---

    function choiceEntries() {
        const spec = edits.spec(nodeId, name)
        const current = typeof literal === "string" ? literal : spec["default"]
        return (spec.choices || []).map(c => ({ text: c, value: c, checked: c === current }))
    }
    // The canvas picker (#211): this root's canvases and the two ways to make one, the rows every canvas menu has
    // (canvasMenu.js); a stored id this root has none of heads the list, checked and dimmed, and says so: the list is
    // still there to replace it. A Canvas Region can also drag its rectangle out on the canvas (#213).
    function canvasEntries() {
        const entries = CanvasMenu.entries(edits.canvases(), canvasId, canvasMissing ? canvasId : "", true)
        if (edits.regionState(nodeId).ok === true)
            entries.push({ text: qsTr("Pick region…"), value: { action: "region" }, keep: true,
                           enabled: !canvasMissing,
                           note: qsTr("Drag the rectangle on the canvas instead of typing X, Y, Width and Height.") })
        return entries
    }
    function seedEntries() {
        const mode = edits.seedMode(nodeId, name)
        return [{ text: qsTr("Fixed"), value: "fixed", checked: mode === "fixed" },
                { text: qsTr("Randomize each run"), value: "random", checked: mode === "random" },
                { text: qsTr("Increment after each run"), value: "increment", checked: mode === "increment" }]
    }
    function modelEntries() {
        const entries = []
        const spec = edits.spec(nodeId, name)
        const current = literal !== undefined ? literal.name : undefined
        const currentHash = literal !== undefined ? literal.hash : undefined
        if (spec.optional)
            entries.push({ text: qsTr("Any ready model"), value: { unset: true }, checked: current === undefined })
        // Grouped by provider instance (this machine, a paired host), each under its name. A
        // model that is not ready says so, and its note says how to make it ready: Fugue never
        // starts one by itself (8.4). Then the files on disk, under the folder each is in.
        // Judged files (#85, #86): for a text encoder, VAE or LoRA of a picked diffusion model,
        // those that fit it first under "Fits <model>", the others under "Other", dimmed when
        // they do not fit (one whose header tells nothing is not); for a diffusion model, the
        // files the helper cannot load under "Other", dimmed, with why. Offered models keep
        // their place.
        let listed = edits.models(nodeId, name)
        // The catalog's models not on disk come last, under "Download" (#134).
        const downloadable = listed.filter(m => m.catalog === true)
        listed = listed.filter(m => m.catalog !== true)
        // The family filter (#370): files of one model family (engine/model_family.h), the providers' models
        // and files of no family always. Offered when the files make two or more families, or one is chosen.
        const families = familiesOf(listed)
        if (familyFilter !== "" && !families.includes(familyFilter))
            familyFilter = ""
        // Its row is last, beside "Type a name…": the first row stays the file Enter picks.
        const familyRow = families.length > 1 || familyFilter !== ""
                          ? { text: qsTr("Family: %1").arg(familyFilter !== "" ? familyFilter : qsTr("All")), value: { familyMenu: true }, keep: true }
                          : null
        if (familyFilter !== "")
            listed = listed.filter(m => m.path === undefined || m.family === familyFilter)
        // Files of a family sit together, under its name, their quantizations beside each other.
        const byFamily = (a, b) => (a.path === undefined || b.path === undefined || a.family === b.family) ? 0
                                   : a.family === "" ? 1 : b.family === "" ? -1 : (a.family < b.family ? -1 : 1)
        listed = listed.map((m, i) => ({ m: m, i: i })).sort((a, b) => byFamily(a.m, b.m) || a.i - b.i).map(x => x.m)
        const fitsFor = edits.fitsModel(nodeId, name)
        const anyFits = listed.some(m => m.fits === true)
        const isOther = m => m.path !== undefined && (anyFits ? m.fits !== true : m.fits === false)
        listed = listed.filter(m => !isOther(m)).concat(listed.filter(isOther))
        let group = null
        let other = false
        let fitsShown = false
        for (const m of listed) {
            if (isOther(m) && !other) {
                other = true
                entries.push({ text: qsTr("Other"), heading: true, enabled: false })
            }
            if (anyFits && m.fits === true && !fitsShown) {
                fitsShown = true
                group = m.folder // one heading for them all
                entries.push({ text: qsTr("Fits %1").arg(fitsFor), heading: true, enabled: false })
            }
            const heading = m.path !== undefined ? (m.family ? m.family : m.folder) : m.provider
            if (heading !== group && !other && !(anyFits && m.fits === true)) {
                group = heading
                entries.push({ text: heading, heading: true, enabled: false })
            }
            if (m.path !== undefined) {
                // The file a stored handle names is the one with its hash, not only its name.
                // The recommended file says why (#370), the star is in its detail for a glance.
                const recommended = m.recommended === true
                entries.push({ text: m.title, detail: (recommended ? qsTr("Recommended") + "  " : "") + sizeText(m.size),
                               value: { disk: m.path }, dim: m.fits === false,
                               place: "local", note: m.fitNote ? m.fitNote : (recommended ? m.recommendedWhy : ""),
                               checked: currentHash !== undefined && m.handle.hash === currentHash })
                continue
            }
            // Where it lives (#164): a paired host's model says so, dimmed "host offline" when the
            // host does not answer; one served on this machine is local.
            const status = m.hostOffline ? qsTr("host offline")
                           : m.status === "ready" ? "" : m.status === "loading" ? qsTr("loading") : qsTr("not running")
            entries.push({ text: m.title, detail: status, note: m.note, value: { handle: m.handle },
                           place: m.host !== "" ? "host" : "local", placeHost: m.host, dim: m.hostOffline,
                           checked: m.name === current && currentHash === undefined })
        }
        if (downloadable.length > 0)
            entries.push({ text: qsTr("Download"), heading: true, enabled: false })
        // The row says the size; the note, what it is and its license; a filter also reads `keywords`.
        // Picking one asks first (the license, the source), it never downloads by itself.
        for (const m of downloadable)
            entries.push({ text: m.title, detail: sizeText(m.size), note: m.detail + "\n" + m.note,
                           keywords: m.keywords, value: { download: m.id } })
        if (edits.modelsLoading(nodeId, name))
            entries.push({ text: qsTr("Looking for models…"), enabled: false })
        else if (listed.length === 0 && downloadable.length === 0)
            entries.push({ text: edits.modelsEmptyText(nodeId, name), enabled: false, note: "" })
        if (familyRow)
            entries.push(familyRow)
        entries.push({ text: qsTr("Type a name…"), value: { typeName: true }, keep: true })
        return entries
    }
    // The model families of the listed files, in order of first appearance.
    function familiesOf(files) {
        const out = []
        for (const m of files)
            if (m.path !== undefined && m.family && !out.includes(m.family))
                out.push(m.family)
        return out.sort()
    }
    // The family the model menu is narrowed to; "" shows every file. Kept while the widget lives.
    property string familyFilter: ""
    // The choices of the family menu: All, then each family of the files with its count.
    function familyEntries() {
        const files = edits.models(nodeId, name).filter(m => m.path !== undefined)
        const entries = [{ text: qsTr("All families"), detail: String(files.length), value: { family: "" }, checked: familyFilter === "" }]
        for (const family of familiesOf(files))
            entries.push({ text: family, detail: String(files.filter(m => m.family === family).length), value: { family: family },
                           checked: family === familyFilter })
        return entries
    }
    // A model file's size as people read it: "6.2 GB", "335 MB".
    function sizeText(bytes) {
        const gb = bytes / 1e9
        return gb >= 1 ? qsTr("%1 GB").arg(gb.toFixed(1)) : qsTr("%1 MB").arg(Math.max(1, Math.round(bytes / 1e6)))
    }
    // A Load Image's picker: the file browser, then the images last pasted or dropped.
    function inputEntries() {
        const entries = [{ text: qsTr("Browse files…"), value: { browse: true } }]
        for (const input of edits.recentInputs())
            entries.push({ text: input.name, thumbnail: input.url, value: { recent: input.path } })
        return entries
    }
    function picked(value) {
        if (kind === "file") {
            if (value.browse)
                return openDialog()
            return edits.useInput(nodeId, value.recent)
        }
        switch (kind) {
        case "canvas":
            if (value.action === "new")
                return picker().askNewCanvas()
            if (value.action === "import")
                return picker().askPicture()
            if (value.action === "region")
                return canvasView ? canvasView.openRegion(nodeId) : undefined
            if (value.active)
                return edits.setValue(nodeId, name, "")
            return edits.setValue(nodeId, name, value.id)
        case "choice":
            return edits.setValue(nodeId, name, value)
        case "seed":
            return edits.setSeedMode(nodeId, name, value)
        case "model":
            if (value.unset)
                return edits.unset(nodeId, name)
            if (value.typeName)
                return openEditor()
            if (value.familyMenu)
                return openMenu(familyEntries(), "")
            if (value.family !== undefined) {
                familyFilter = value.family
                return openMenu(null, "")
            }
            if (value.download !== undefined)
                return picker().confirmDownload(value.download)
            if (value.disk !== undefined)
                return edits.setDiskModel(nodeId, name, value.disk)
            return edits.setModel(nodeId, name, value.handle)
        }
    }

    // Menus and pickers live in WidgetPicker.qml, loaded on the first use: they need Qt Quick
    // Controls and Dialogs, which a widget at rest should not pay for.
    property Item pickers: null
    function picker() {
        if (!pickers)
            pickers = Qt.createComponent("WidgetPicker.qml").createObject(root, { host: root })
        return pickers
    }
    // `entries` null: the model list, kept current while models arrive. `follow` names what else
    // keeps the open menu current: "canvases".
    function openMenu(entries, follow) { picker().openMenu(entries, follow || "") }
    function openDialog() { picker().openDialog() }
}
