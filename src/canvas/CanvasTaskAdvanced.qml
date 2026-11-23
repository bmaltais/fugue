pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "widgets"

// The task bar's Advanced popover (#231 slice 4, docs/canvas-actions-design.md 1.4 and 4), hung from the bar's Advanced
// button. It lists what the action's block binds that the bar has no field for (Strength, Model, Steps, the plain inputs),
// each drawn with the Graph's own InputWidget over the actions' form model (CanvasActions.advanced: the model picker with
// its Download rows, the drag number, the choice), so a value here is the value a node face would take. An input of the
// block with `choice_values` (Refine's Strength) is a row of its words instead. Then Count (1 to 4, for an action with a
// seed), and Edit workflow. What is set here is remembered per action and per root (canvas-actions.json) and Generate uses it;
// Reset puts the action's own values back. The popover is transient: it closes with Escape or a press outside it (a
// picker's menu is modal and on top, so a click in it does not).
Popup {
    id: pop
    objectName: "canvasTaskAdvanced"
    required property CanvasLook look
    // CanvasActions, and the action the bar would run (its row: id, name, hasSeed).
    property var actions: null
    property var current: ({})
    // The rows of CanvasActions.advanced(), read when it opens.
    property var rows: []
    property int count: 1
    // Bumped by anything that changes what a row shows (a choice, Reset).
    property int tick: 0
    signal editWorkflow()

    readonly property real u: look.unit
    readonly property var form: actions ? actions.form : null
    readonly property bool changed: tick >= 0 && actions !== null && (current.id ?? "") !== "" && actions.hasAdvancedChanges(current.id)
    readonly property bool hasSeed: current.hasSeed === true

    parent: Overlay.overlay
    padding: 2 * u
    width: 76 * u
    modal: false
    focus: true
    // No fade or scale, as the menus have none (docs/canvas-look.md 15).
    enter: null
    exit: null
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: PopupSurface { look: pop.look }

    // The button it hangs from while it is open.
    property Item anchorButton: null
    // Under the button, its right edge on the button's, kept inside the window; above it when there is no room.
    function place() {
        if (!anchorButton)
            return
        const at = anchorButton.mapToItem(Overlay.overlay, 0, anchorButton.height)
        x = Math.max(4 * u, Math.min(Overlay.overlay.width - 4 * u - width, at.x + anchorButton.width - width))
        const below = at.y + u
        y = below + height <= Overlay.overlay.height - 4 * u ? below : Math.max(4 * u, at.y - anchorButton.height - u - height)
    }
    function openFor(button: Item) {
        if (!actions)
            return
        rows = actions.advanced(current.id ?? "")
        count = actions.variations(current.id ?? "")
        tick++
        anchorButton = button
        place()
        open()
    }
    onOpened: place() // its height is known now
    onHeightChanged: if (opened) place()

    contentItem: ColumnLayout {
        spacing: pop.u
        RowLayout {
            Layout.fillWidth: true
            spacing: pop.u
            Text {
                objectName: "canvasTaskAdvancedTitle"
                Layout.fillWidth: true
                elide: Text.ElideRight
                color: pop.look.text
                font.pixelSize: pop.look.labelSize
                font.bold: true
                text: qsTr("%1 settings").arg(pop.current.name ?? "")
            }
            ViewButton {
                objectName: "canvasTaskAdvancedReset"
                look: pop.look
                compact: true
                flat: true
                visible: pop.changed
                text: qsTr("Reset")
                tip: qsTr("Put this action's own values back (Count stays)")
                onTapped: { pop.actions.resetAdvanced(pop.current.id); pop.rows = pop.actions.advanced(pop.current.id); pop.tick++ }
            }
        }
        Text {
            objectName: "canvasTaskAdvancedNone"
            Layout.fillWidth: true
            visible: pop.rows.length === 0
            wrapMode: Text.Wrap
            color: pop.look.label
            font.pixelSize: pop.look.labelSize
            text: qsTr("This action has no other settings. Edit workflow shows what it does.")
        }
        Repeater {
            model: pop.rows
            delegate: Item {
                id: entry
                required property var modelData
                readonly property bool isChoice: modelData.kind === "choice"
                objectName: "canvasTaskAdvancedRow-" + modelData.key
                Layout.fillWidth: true
                Layout.preferredHeight: 7 * pop.u
                // A choice's words, a segmented row; the one chosen is the quiet marking.
                Row {
                    visible: entry.isChoice
                    anchors.fill: parent
                    spacing: pop.u
                    Text {
                        width: pop.availableWidth * 0.3 - pop.u
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        color: pop.look.label
                        font.pixelSize: pop.look.labelSize
                        text: entry.modelData.label
                    }
                    Repeater {
                        model: entry.isChoice ? entry.modelData.choices : []
                        delegate: ViewButton {
                            id: word
                            required property string modelData
                            objectName: "canvasTaskAdvancedChoice-" + entry.modelData.key + "-" + modelData
                            look: pop.look
                            text: modelData
                            current: pop.tick >= 0 && pop.actions.choiceOf(pop.current.id, entry.modelData.key) === modelData
                            checkable: true
                            accented: false
                            onTapped: { pop.actions.setChoice(pop.current.id, entry.modelData.key, modelData); pop.tick++ }
                        }
                    }
                }
                // Anything else: the Graph's widget over the form, its label drawn over the well's left part as a node face does.
                Item {
                    visible: !entry.isChoice
                    anchors.fill: parent
                    InputWidget {
                        objectName: "canvasTaskAdvancedWidget-" + entry.modelData.key
                        anchors.fill: parent
                        edits: pop.form ? pop.form.widgets : null
                        nodeId: entry.modelData.node
                        port: pop.form && edits && edits.graphRevision >= 0 ? pop.form.inputPort(entry.modelData.node, entry.modelData.input) : ({})
                        look: pop.look
                    }
                    Text {
                        x: 2 * pop.u
                        width: parent.width * 0.45 - 3 * pop.u
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        color: pop.look.label
                        font.pixelSize: pop.look.labelSize
                        text: entry.modelData.label
                        textFormat: Text.PlainText
                    }
                }
            }
        }
        // Count: one Generate makes this many runs of the same picture and selection, each its own layer; the first is shown.
        RowLayout {
            objectName: "canvasTaskCountRow"
            Layout.fillWidth: true
            visible: pop.hasSeed
            spacing: pop.u
            Text {
                Layout.preferredWidth: pop.availableWidth * 0.3 - pop.u
                color: pop.look.label
                font.pixelSize: pop.look.labelSize
                text: qsTr("Count")
            }
            Repeater {
                model: [1, 2, 3, 4]
                delegate: ViewButton {
                    required property int modelData
                    objectName: "canvasTaskCount-" + modelData
                    look: pop.look
                    text: String(modelData)
                    current: pop.count === modelData
                    checkable: true
                    accented: false
                    tip: modelData === 1 ? qsTr("One layer per Generate")
                                         : qsTr("%n layers per Generate: the first is shown, the others are hidden", "", modelData)
                    onTapped: { pop.actions.setVariations(pop.current.id, modelData); pop.count = modelData }
                }
            }
        }
        ViewButton {
            objectName: "canvasTaskEdit"
            look: pop.look
            Layout.fillWidth: true
            flat: true
            enabled: pop.actions !== null && (pop.current.id ?? "") !== ""
            text: qsTr("Edit workflow")
            tip: qsTr("Open %1 as a workflow in the Graph").arg(pop.current.name ?? "")
            onTapped: { pop.close(); pop.editWorkflow() }
        }
    }
}
