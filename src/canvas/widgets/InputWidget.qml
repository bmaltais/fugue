pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import ".."

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
    property Item canvasView    // takes the focus back when an edit ends
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
    readonly property string valueText: {
        if (linked)
            return edits.source(nodeId, name)
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
        : kind === "choice" || kind === "model" ? "chevron-down"
        : kind === "seed" ? "dices"
        : kind === "folder" ? "folder"
        : kind === "file" ? "file" : ""
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
        color: root.hashing.error !== undefined ? root.look.error
               : root.linked || root.isDefault || root.kind === "model" && root.literal === undefined
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
        case "choice":
            return openMenu(choiceEntries())
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
        // A text encoder, VAE or LoRA of a picked diffusion model (#85, #86): the files that fit
        // it first, under their folders, then the other files under one "Other" heading, those that
        // do not fit dimmed (one whose header tells nothing is not). Offered models keep their
        // place.
        let listed = edits.models(nodeId, name)
        const fitsKnown = listed.some(m => m.fits !== undefined)
        const isOther = m => fitsKnown && m.path !== undefined && m.fits !== true
        if (fitsKnown)
            listed = listed.filter(m => !isOther(m)).concat(listed.filter(isOther))
        let group = null
        let other = false
        for (const m of listed) {
            if (isOther(m) && !other) {
                other = true
                entries.push({ text: qsTr("Other"), heading: true, enabled: false })
            }
            const heading = m.path !== undefined ? m.folder : m.provider
            if (heading !== group && !other) {
                group = heading
                entries.push({ text: heading, heading: true, enabled: false })
            }
            if (m.path !== undefined) {
                // The file a stored handle names is the one with its hash, not only its name.
                entries.push({ text: m.title, detail: sizeText(m.size), value: { disk: m.path }, dim: m.fits === false,
                               checked: currentHash !== undefined && m.handle.hash === currentHash })
                continue
            }
            const status = m.status === "ready" ? "" : m.status === "loading" ? qsTr("loading") : qsTr("not running")
            entries.push({ text: m.title, detail: status, note: m.note, value: { handle: m.handle },
                           checked: m.name === current && currentHash === undefined })
        }
        if (edits.modelsLoading(nodeId, name))
            entries.push({ text: qsTr("Looking for models…"), enabled: false })
        else if (listed.length === 0)
            entries.push({ text: edits.modelsEmptyText(nodeId, name), enabled: false, note: "" })
        entries.push({ text: qsTr("Type a name…"), value: { typeName: true }, keep: true })
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
        case "choice":
            return edits.setValue(nodeId, name, value)
        case "seed":
            return edits.setSeedMode(nodeId, name, value)
        case "model":
            if (value.unset)
                return edits.unset(nodeId, name)
            if (value.typeName)
                return openEditor()
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
    // `entries` null: the model list, kept current while models arrive.
    function openMenu(entries) { picker().openMenu(entries) }
    function openDialog() { picker().openDialog() }
}
