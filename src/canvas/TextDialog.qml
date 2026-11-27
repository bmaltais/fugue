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
Popup {
    id: root
    objectName: "textDialog"
    readonly property CanvasLook look: App.theme.look
    required property WidgetEdit edits
    required property CanvasModel canvasModel
    property string nodeId
    property string inputName

    // The node's text inputs ([{name, label, widget, linked}]) and the one shown.
    property var inputs: []
    readonly property int index: {
        for (let i = 0; i < inputs.length; ++i)
            if (inputs[i].name === inputName)
                return i
        return -1
    }
    readonly property var current: index >= 0 ? inputs[index] : ({ name: "", label: "", widget: "line", linked: false })
    readonly property bool linked: current.linked
    // Re-read the linked input's source when the run state or the document changes.
    property int stamp: 0
    readonly property var source: stamp >= 0 && root.opened && linked ? edits.linkedText(nodeId, inputName) : ({ linked: false })
    readonly property string title: canvasModel.nodeTitle(nodeId) + " · " + current.label
    readonly property string shownText: linked ? (source.has ? source.text : "") : edit.text
    // The text is being typed or set by the window itself: a document change it made is not an outside edit.
    property bool busy: false
    // Why the text typed is not a value, and the node's own error under it (a run's or a check's).
    property string typedError: ""
    readonly property string nodeError: stamp >= 0 && root.opened ? canvasModel.runError(nodeId) : ""

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

    function inputList() { return edits.textInputs(nodeId) }

    function openFor(node, input) {
        nodeId = node
        inputs = inputList()
        const first = inputs.find(i => i.widget === "multiline") || inputs[0]
        const wanted = inputs.find(i => i.name === input) || first
        if (!wanted)
            return
        if (opened)
            finishSession()
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
        busy = false
    }
    function finishSession() {
        edits.finishInto(nodeId, inputName)
    }
    // Tab (1) and Shift+Tab (-1): the next text input of the node. With one only, the focus moves on in the window.
    function step(by) {
        if (inputs.length < 2) {
            const next = edit.nextItemInFocusChain(by > 0)
            if (next)
                next.forceActiveFocus()
            return
        }
        finishSession()
        inputName = inputs[(index + by + inputs.length) % inputs.length].name
        load()
        edit.forceActiveFocus()
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
    Connections {
        target: root.canvasModel.nodes
        function onDataChanged() { root.refresh() }
    }

    background: PopupSurface { look: root.look; tone: "slate" }

    contentItem: Item {
        // Escape closes from any control of the window, not only the text field (the field closes it itself first). Not Popup's own
        // closePolicy: with CloseOnEscape Qt turns the window's shortcuts off while the popup is open, and Ctrl+Enter must still run.
        Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape
        Keys.onEscapePressed: root.close()
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
                    readOnly: root.linked
                    selectByMouse: true
                    textFormat: TextEdit.PlainText
                    color: root.linked && !root.source.has ? root.look.label : root.look.text
                    selectionColor: root.look.accent
                    selectedTextColor: root.look.accentForeground
                    font.pixelSize: root.textPx
                    onTextChanged: {
                        if (root.busy || root.linked)
                            return
                        root.busy = true
                        root.typedError = root.edits.typeInto(root.nodeId, root.inputName, text)
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
            readonly property string problem: root.typedError !== "" ? root.typedError : root.nodeError
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
