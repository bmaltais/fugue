pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../kit"

// The text window (#715, spec 6.2): a node's text input in a larger, movable and resizable window, for reading and editing a long
// prompt at a comfortable size at any canvas zoom. A pop-up in LargeViewDialog's family (the same surface and bar button) but not
// modal: the canvas behind it keeps its mouse and keeps updating while a run goes, and Ctrl+Enter runs the workflow from inside it.
// `openFor(node, input)` opens it (the expand button of a multiline field, Alt+T, the node menu's Edit text…); it is made on the first
// use and keeps its place and size afterwards.
//
// The text is the input's literal, written as it is typed through WidgetEdit's session (`typeInto`, edit.h), so a visit is one undo step
// for the input; Tab goes to the node's next text input (Shift+Tab back), which ends that input's step as the inline editor's Tab does,
// and the title follows. Escape or the close button ends it and keeps the text (a prompt is never lost to a key). Another edit made on
// the canvas meanwhile (a run advancing a seed, an undo) ends the session; the next key begins a new one, so no text goes to the
// wrong input. A linked input shows the text its source made in its last run, read-only, with Go to source.
//
// It has three kinds of subject (#715 slice 5), set by the call that opens it: a node's text input (`openFor`, above), a node's text
// result (`openForResult`: a double-click on its excerpt; the whole text, read-only, following the node's runs), and an App form's text
// field (`openForField`: the form's own value, `AppView.setValue` as it is typed; the form has no undo, so there is no step to keep and
// nothing to end, and Escape keeps the text as everywhere).
Popup {
    id: root
    objectName: "textDialog"
    readonly property CanvasLook look: App.theme.look
    property WidgetEdit edits: null
    property CanvasModel canvasModel: null
    property AppView appView: null
    // "input" (a node's text input), "result" (a node's text result, read-only) or "form" (an App form's field).
    property string mode: ""
    property string nodeId
    property string inputName
    // The App form's field being edited ({key, label, multiline}): a field that is not `multiline` is one line that wraps, as in the form.
    property var field: ({})
    // A form's field that is not `multiline`, and a node's `line` input (Tab and Edit text… reach those too): one line, no Enter.
    readonly property bool singleLine: mode === "form" ? field.multiline !== true : mode === "input" && current.widget === "line"

    // The subject's text inputs ([{name, label, widget, linked, syntax}]: a node's, or the one result or form field) and the one shown.
    property var inputs: []
    readonly property int index: {
        for (let i = 0; i < inputs.length; ++i)
            if (inputs[i].name === inputName)
                return i
        return -1
    }
    readonly property var current: index >= 0 ? inputs[index] : ({ name: "", label: "", widget: "line", linked: false, syntax: "" })
    readonly property bool linked: current.linked
    // Nothing is typed: a linked input shows its source's text, and a result is only read.
    readonly property bool readOnly: linked || mode === "result"
    // Re-read the linked input's source when the run state or the document changes.
    property int stamp: 0
    readonly property var source: stamp >= 0 && root.opened && linked ? edits.linkedText(nodeId, inputName) : ({ linked: false })
    readonly property string title: mode === "form" || !canvasModel ? current.label : canvasModel.nodeTitle(nodeId) + " · " + current.label
    readonly property string shownText: linked ? (source.has ? source.text : "") : edit.text
    // The text is being typed or set by the window itself: a document change it made is not an outside edit.
    property bool busy: false
    // The text as last loaded or typed: a change that leaves it so is the highlighter's formatting, not a key.
    property string known: ""
    // Why the text typed is not a value, and the node's own error under it (a run's or a check's).
    property string typedError: ""
    readonly property string nodeError: mode === "input" && stamp >= 0 && root.opened ? canvasModel.runError(nodeId) : ""

    readonly property real u: look.unit
    readonly property int textPx: Math.round(look.textSize * 14 / 12) // the 14 px interface text at the default size
    readonly property real lineRatio: 1.5

    // The place and size it keeps between visits; the window may be smaller than they say.
    property real wantWidth: Math.round(ruler.width + 2 * padding + 2 * 2 * u + 2.5 * u)
    property real wantHeight: Math.round(0.62 * (parent ? parent.height : 600))
    readonly property real minWidth: 40 * u
    readonly property real minHeight: 36 * u

    parent: Overlay.overlay
    width: Math.max(minWidth, Math.min(wantWidth, parent.width - 2 * u))
    height: Math.max(minHeight, Math.min(wantHeight, parent.height - 2 * u))
    padding: 2 * u
    modal: false
    dim: false
    focus: true
    enter: null
    exit: null
    closePolicy: Popup.NoAutoClose

    TextMetrics {
        id: ruler
        font.pixelSize: root.textPx
        text: "n".repeat(72) // about 72 characters wide
    }

    // The text inputs of a subject: of mode `m` on `node`, or of the form's `formField`.
    function inputsOf(m, node, formField) {
        if (m === "form")
            return [{ name: formField.key, label: formField.label, widget: "multiline", linked: false, syntax: "" }]
        if (m === "result") {
            const result = canvasModel.resultText(node)
            return result.has ? [{ name: result.output, label: result.label, widget: "multiline", linked: false, syntax: "" }] : []
        }
        return edits.textInputs(node)
    }
    function inputList() { return inputsOf(mode, nodeId, field) }

    // The text window on a node's text input (`input` empty: its first multiline one).
    function openFor(node, input) { show("input", node, input, null, {}) }
    // ... on the whole text a node's last run made (read-only).
    function openForResult(node) { show("result", node, "", null, {}) }
    // ... on an App form's text field, `field` being the form's own ({key, label, multiline}).
    function openForField(form, formField) { show("form", "", formField.key, form, formField) }
    // The visit on show ends first, while nodeId, inputName and mode are still its: its undo step is closed before another subject loads.
    function retire() {
        if (opened)
            finishSession()
    }
    // A subject with no text input leaves the window as it is.
    function show(newMode, node, input, form, formField) {
        const list = inputsOf(newMode, node, formField)
        const wanted = list.find(i => i.name === input) || list.find(i => i.widget === "multiline") || list[0]
        if (!wanted)
            return
        retire()
        mode = newMode
        if (form)
            appView = form
        field = formField
        nodeId = node
        inputs = list
        inputName = wanted.name
        load()
        if (!opened) {
            x = Math.round((parent.width - width) / 2)
            y = Math.round((parent.height - height) / 2)
            open()
        }
        edit.forceActiveFocus()
    }
    // The text window's text from the document.
    function startText() {
        if (mode === "form") {
            const kept = appView.valueOf(inputName)
            return kept === undefined || kept === null ? "" : String(kept)
        }
        if (mode === "result")
            return canvasModel.resultText(nodeId).text ?? ""
        const value = edits.value(nodeId, inputName)
        if (typeof value === "string")
            return value
        const given = edits.spec(nodeId, inputName)["default"]
        return typeof given === "string" ? given : ""
    }
    function load() {
        busy = true
        typedError = ""
        edit.text = linked ? (source.has ? source.text : "") : startText()
        TextLines.setLineHeight(edit.textDocument, lineRatio)
        edit.cursorPosition = edit.length
        known = edit.text
        busy = false
    }
    // The text as typed, to where this subject keeps it; returns why not (the document's own reason), or empty.
    function type(text) {
        if (mode === "form") {
            appView.setValue(inputName, text)
            return ""
        }
        return edits.typeInto(nodeId, inputName, text)
    }
    function finishSession() {
        if (mode === "input")
            edits.finishInto(nodeId, inputName)
    }
    // Tab (1) and Shift+Tab (-1): the next text input of the node. With one only, the focus moves on in the window.
    // Past the last input (or before the first) it goes on to the window's buttons, and Tab from them comes back to the text.
    function step(by) {
        const edge = by > 0 ? index === inputs.length - 1 : index === 0
        if (inputs.length < 2 || edge) {
            if (inputs.length >= 2)
                finishSession()
            const list = buttons()
            if (list.length > 0)
                list[by > 0 ? 0 : list.length - 1].forceActiveFocus(by > 0 ? Qt.TabFocusReason : Qt.BacktabFocusReason)
            return
        }
        finishSession()
        inputName = inputs[(index + by + inputs.length) % inputs.length].name
        load()
        edit.forceActiveFocus()
    }
    // The window's buttons in Tab order, and Tab on one: the next button; past the last (or before the first) the text again.
    function buttons() {
        return [goButton, closeButton].filter(b => b.visible)
    }
    function walkButtons(forward) {
        const list = buttons()
        const at = list.indexOf(root.contentItem.Window.activeFocusItem)
        if (at < 0) { // the focused button went (Go to source after a link was taken out): the text takes the focus
            edit.forceActiveFocus()
            return
        }
        const to = at + (forward ? 1 : -1)
        ;(to >= 0 && to < list.length ? list[to] : edit).forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
    }
    // The node, its input or its link changed under the window: follow, or close when the input is gone.
    function refresh() {
        if (!opened)
            return
        inputs = inputList()
        if (index < 0) {
            close()
            return
        }
        stamp++
        // The text the window holds follows the document (an undo, a link made) unless it is being typed, or holds a text the
        // document refused (the person's own, kept with its error).
        if (busy || typedError !== "")
            return
        const text = linked ? (source.has ? source.text : "") : startText()
        if (text !== edit.text)
            load()
    }

    onClosed: {
        finishSession()
        busy = true // emptying the field is not a typed change
        edit.text = ""
        known = ""
        busy = false
    }
    Connections {
        target: root.edits
        function onGraphChanged() { root.refresh() }
    }
    Connections {
        target: root.canvasModel
        function onScopeChanged() { root.refresh() }
        function onDocumentReset() { root.close() }
    }
    // A run or an edit anywhere else on the canvas does not touch this text: only the rows of the node shown, and of the node a linked
    // input takes its text from, are looked at (a result of up to 200,000 characters is not copied for every other node's progress).
    Connections {
        target: root.mode !== "form" && root.canvasModel ? root.canvasModel.nodes : null
        function onDataChanged(topLeft, bottomRight) {
            if (!root.opened)
                return
            const watched = [root.nodeId, root.source.source || ""]
            const nodes = root.canvasModel.nodes
            for (let row = topLeft.row; row <= bottomRight.row; ++row)
                if (watched.includes(nodes.idAt(row))) {
                    root.refresh()
                    return
                }
        }
    }
    // The form's value changed by itself (Reuse inputs, a sample, another tab) or in the field under the window: the window follows, unless
    // it is being typed.
    Connections {
        target: root.appView
        function onValueChanged(key) { if (key === root.inputName) root.refresh() }
        function onValueTyped(key) { if (key === root.inputName) root.refresh() }
        // Another tab, or the form rebuilt: the field this window is typing into is not the shown form's any more.
        function onAppChanged() { if (root.mode === "form" && root.opened) root.close() }
    }

    background: PopupSurface { look: root.look; tone: "slate" }

    contentItem: Item {
        // Escape closes from any control of the window, not only the text field (the field closes it itself first). Not Popup's own
        // closePolicy: with CloseOnEscape Qt turns the window's shortcuts off while the popup is open, and Ctrl+Enter must still run.
        // In fact every key without Ctrl, Alt or Meta (the rule of a text field, CommandRegistry::belongsToTextField): with the focus on a
        // button no text field holds it, and Delete, Q, P and the rest would act on the canvas behind this non-modal window. Ctrl+Enter
        // (Run) is left to the shortcuts, Return and Space still press the focused button.
        Keys.onShortcutOverride: event => {
            const mods = event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)
            event.accepted = event.key === Qt.Key_Escape || (mods === 0 && !(event.key >= Qt.Key_F1 && event.key <= Qt.Key_F35))
        }
        Keys.onEscapePressed: root.close()
        Keys.onTabPressed: root.walkButtons(true)
        Keys.onBacktabPressed: root.walkButtons(false)
        // Right to left: the bar and the footer mirror, the text is its own.
        readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
        Item {
            id: bar
            objectName: "textBar"
            width: parent.width
            height: 7 * root.u
            LayoutMirroring.enabled: parent.rightToLeft
            LayoutMirroring.childrenInherit: true
            // Drag the bar to move the window.
            MouseArea {
                id: move
                anchors.fill: parent
                property point start
                property point from
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.ArrowCursor
                onPressed: mouse => {
                    start = mapToItem(null, mouse.x, mouse.y)
                    from = Qt.point(root.x, root.y)
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const at = mapToItem(null, mouse.x, mouse.y)
                    root.x = Math.max(0, Math.min(root.parent.width - root.width, from.x + at.x - start.x))
                    root.y = Math.max(0, Math.min(root.parent.height - root.height, from.y + at.y - start.y))
                }
            }
            Text {
                objectName: "textTitle"
                anchors.left: parent.left
                anchors.leftMargin: root.u
                anchors.right: closeButton.left
                anchors.rightMargin: 2 * root.u
                anchors.verticalCenter: parent.verticalCenter
                text: root.title
                elide: Text.ElideRight
                color: root.look.text
                font.pixelSize: root.look.textSize
                font.weight: Font.Medium
            }
            ViewButton {
                id: closeButton
                objectName: "textClose"
                anchors.right: parent.right
                look: root.look
                glyph: "x"
                tip: qsTr("Close (Esc)")
                onTapped: root.close()
            }
        }

        // What a linked input is: where the text comes from, with Go to source.
        Item {
            id: linkLine
            objectName: "textLinkLine"
            visible: root.linked
            anchors.top: bar.bottom
            anchors.topMargin: root.u
            width: parent.width
            height: visible ? 7 * root.u : 0
            LayoutMirroring.enabled: parent.rightToLeft
            LayoutMirroring.childrenInherit: true
            ViewButton {
                id: goButton
                objectName: "textGoToSource"
                anchors.right: parent.right
                look: root.look
                text: qsTr("Go to source")
                tip: qsTr("Select %1 on the canvas and bring it into view").arg(root.source.sourceTitle || "")
                accented: false
                onTapped: root.canvasModel.reveal(root.source.source)
            }
            Text {
                objectName: "textLinkNote"
                anchors.left: parent.left
                anchors.leftMargin: root.u
                anchors.right: goButton.left
                anchors.rightMargin: 2 * root.u
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideRight
                color: root.look.label
                font.pixelSize: root.look.labelSize
                text: root.source.has
                      ? qsTr("From %1 · %2, as it made it in its last run. Take the link out to type here.").arg(root.source.sourceTitle).arg(root.source.output)
                      : qsTr("%1 · %2 has not made any text yet: run the workflow and it shows here.").arg(root.source.sourceTitle).arg(root.source.output)
            }
        }

        Rectangle {
            id: well
            objectName: "textWell"
            anchors.top: root.linked ? linkLine.bottom : bar.bottom
            anchors.topMargin: root.u
            anchors.bottom: footer.top
            anchors.bottomMargin: root.u
            width: parent.width
            color: root.look.well
            radius: root.look.smallCorner
            border.width: edit.activeFocus ? 1 : 0
            border.color: root.look.accent

            // The manifest's `syntax` of the input colours the text (spec 6.2); a linked input's text is a result, not a template.
            PromptHighlight {
                id: highlight
                objectName: "textHighlight"
                target: edit.textDocument
                syntax: root.linked ? "" : (root.current.syntax || "")
                accent: root.look.accent
                label: root.look.label
                role: root.look.role("blue").frameEdge
                error: root.look.error
                well: root.look.well
            }

            Flickable {
                id: flick
                anchors.fill: parent
                anchors.margins: 2 * root.u
                contentWidth: width
                contentHeight: edit.contentHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentHeight > height
                ScrollBar.vertical: FugueScrollBar { look: root.look }

                TextEdit {
                    id: edit
                    objectName: "textArea"
                    width: flick.width - (flick.ScrollBar.vertical as FugueScrollBar).inset
                    wrapMode: TextEdit.Wrap
                    readOnly: root.readOnly
                    selectByMouse: true
                    textFormat: TextEdit.PlainText
                    color: root.linked && !root.source.has ? root.look.label : root.look.text
                    Accessible.role: Accessible.EditableText
                    Accessible.name: root.current.label
                    selectionColor: root.look.accent
                    selectedTextColor: root.look.accentForeground
                    font.pixelSize: root.textPx
                    onTextChanged: {
                        // The highlighter's own formatting also reports a change of the text, with the text as it was: not typing.
                        if (root.busy || root.readOnly || text === root.known)
                            return
                        root.busy = true
                        // A one-line value of an App form: a pasted line break is a space, as in the form's own field.
                        // What was just inserted loses its breaks, as part of the same undo step (the line height and the history stay; the
                        // breaks of a stored value, outside it, are not touched).
                        if (root.singleLine && /[\r\n]/.test(text)) {
                            const at = cursorPosition
                            let from = 0
                            while (from < root.known.length && from < text.length && text[from] === root.known[from])
                                ++from
                            let tail = 0
                            while (tail < root.known.length - from && tail < text.length - from
                                   && text[text.length - 1 - tail] === root.known[root.known.length - 1 - tail])
                                ++tail
                            TextLines.replaceBreaks(textDocument, from, text.length - tail)
                            cursorPosition = Math.min(at, length)
                        }
                        root.known = edit.text
                        root.typedError = root.type(edit.text)
                        root.busy = false
                    }
                    onCursorRectangleChanged: {
                        if (cursorRectangle.y < flick.contentY)
                            flick.contentY = cursorRectangle.y
                        else if (cursorRectangle.y + cursorRectangle.height > flick.contentY + flick.height)
                            flick.contentY = cursorRectangle.y + cursorRectangle.height - flick.height
                    }
                    // Escape is the window's, before any shortcut on it (run.cancel while a run is going). Ctrl+Enter is left to the
                    // window's Run shortcut: the field takes it as nothing, and the dialog stays open.
                    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape
                    Keys.onEscapePressed: root.close()
                    Keys.onReturnPressed: event => { event.accepted = root.singleLine && !(event.modifiers & Qt.ControlModifier) }
                    Keys.onEnterPressed: event => { event.accepted = root.singleLine && !(event.modifiers & Qt.ControlModifier) }
                    Keys.onTabPressed: event => root.step(event.modifiers & Qt.ShiftModifier ? -1 : 1)
                    Keys.onBacktabPressed: root.step(-1)
                }
            }
        }

        Item {
            id: footer
            objectName: "textFooter"
            anchors.bottom: parent.bottom
            width: parent.width
            height: 4 * root.u
            LayoutMirroring.enabled: parent.rightToLeft
            LayoutMirroring.childrenInherit: true
            readonly property string text: root.shownText
            readonly property int characters: text.length
            readonly property int words: text.trim() === "" ? 0 : text.trim().split(/\s+/).length
            readonly property int lines: text === "" ? 0 : text.split("\n").length
            // What the document refused, else the first thing wrong with the syntax (its line in the words), else the node's own error.
            readonly property string problem: root.typedError !== "" ? root.typedError : highlight.problem !== "" ? highlight.problem : root.nodeError
            Text {
                id: counts
                objectName: "textCounts"
                anchors.left: parent.left
                anchors.leftMargin: root.u
                anchors.verticalCenter: parent.verticalCenter
                color: root.look.label
                font.pixelSize: root.look.labelSize
                text: qsTr("%1 · %2 · %3").arg(qsTr("%n characters", "", footer.characters))
                        .arg(qsTr("%n words", "", footer.words)).arg(qsTr("%n lines", "", footer.lines))
            }
            Text {
                objectName: "textProblem"
                visible: footer.problem !== ""
                anchors.right: grip.left
                anchors.rightMargin: root.u
                anchors.left: counts.right
                anchors.leftMargin: 2 * root.u
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                color: root.look.error
                font.pixelSize: root.look.labelSize
                text: footer.problem
            }
            // Resize by the bottom right corner.
            ResizeMark {
                id: grip
                objectName: "textResize"
                corner: "se"
                nodeCorner: 0
                color: root.look.label
                unit: root.u
                anchors.right: parent.right
                anchors.bottom: parent.bottom
            }
        }

        // The bottom right corner takes the resize: a press there and a drag.
        MouseArea {
            id: resize
            objectName: "textResizeArea"
            width: 5 * root.u
            height: 5 * root.u
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            cursorShape: Qt.SizeFDiagCursor
            preventStealing: true
            property point start
            property real fromWidth
            property real fromHeight
            onPressed: mouse => {
                start = mapToItem(null, mouse.x, mouse.y)
                fromWidth = root.width
                fromHeight = root.height
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const at = mapToItem(null, mouse.x, mouse.y)
                root.wantWidth = Math.max(root.minWidth, Math.min(root.parent.width - root.x, fromWidth + at.x - start.x))
                root.wantHeight = Math.max(root.minHeight, Math.min(root.parent.height - root.y, fromHeight + at.y - start.y))
            }
        }
    }
}
