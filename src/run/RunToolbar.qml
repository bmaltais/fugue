pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../canvas"

// The run toolbar (#60, as ComfyUI's action bar): a bar floating over its parent, anywhere in it
// (#120: dragged by its grip, or moved by the arrow keys), with the batch count, Run (the one highlighted control) and the menu behind it,
// Cancel, and the queue button. It knows nothing of the canvas or of the run queue: the caller
// gives the state (`running`, `pending`, `progress`, `batch`, `queueOpen`) and the rows of the split
// menu (`menuItems`, a CommandMenu's rows), and listens to what a person asked for.
//
//   chunk 1: grip (a MoveGrip: drag, or the arrow keys, Shift for bigger steps; the bar stays inside
//            its parent and is pulled lightly to the edges and the middle)
//   chunk 2: batch count, Run and its menu: `batch` runs are made by each Run, so a seed set to
//            increment gives each its own number
//   chunk 3: Cancel (destructive: its own chunk, 3u before it as between the others)
//   chunk 4: the queue: a toggle for the panel, with the number of runs going and waiting
//
// A 2px bar under it shows the progress of the run going.
Item {
    id: bar
    objectName: "runToolbar"
    required property CanvasLook look
    property bool busy: false       // a run of this window is waiting or going
    property int pending: 0         // runs going and waiting in the queue, of any process
    property real progress: -1      // of the node going, 0 to 1, or -1
    property bool canRun: true
    property bool canCancel: busy
    property int batch: 1
    property int maxBatch: 99
    property bool queueOpen: false
    // Where it floats: fractions (0 to 1) of the room it has in its parent, as MoveGrip keeps them.
    // The default, centered at the top, is {0.5, 0}.
    property point place: Qt.point(0.5, 0)
    property var menuItems: []      // the split menu's rows (CommandMenu `items`)
    readonly property real u: look.unit

    signal menuAboutToOpen
    signal runRequested
    signal cancelRequested
    signal batchRequested(int value)
    signal queueToggled
    signal placeRequested(point place)

    // It fills what it floats over (a Loader does that anyway); only `box` is drawn and takes presses.
    anchors.fill: parent
    // It keeps 4u from the edges of its parent, as it did at rest before it moved freely.
    // The bar itself, for a caller that places something by it.
    readonly property Item surface: box

    // Focus moves to the next control of the bar; at its ends it stays.
    function step(forward) {
        const from = Window.window ? Window.window.activeFocusItem : null
        const next = from ? from.nextItemInFocusChain(forward) : null
        for (let item = next; item; item = item.parent)
            if (item === row) {
                next.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
                return
            }
    }

    // Opens the split menu under the arrow, or over it where there is no room under.
    property var splitMenu: null
    function openMenu() {
        bar.menuAboutToOpen() // the caller gives the rows as they are now
        if (!splitMenu) {
            const component = Qt.createComponent("../menu/CommandMenu.qml")
            splitMenu = component.createObject(bar, {objectName: "runToolbarMenu"})
        }
        splitMenu.items = bar.menuItems
        splitMenu.popupFit(menuButton, 0, menuButton.height, 0, menuButton.height)
    }

    Shadow {
        target: box
        level: 2
        look: bar.look
    }
    Rectangle {
        id: box
        x: grab.position.x
        y: grab.position.y
        width: row.implicitWidth + 2 * bar.u
        height: 9 * bar.u
        radius: Math.min(bar.look.params.cornerCard * bar.u, height / 2)
        color: bar.look.popup
        border.width: 1
        border.color: bar.look.hairline

        Row {
            id: row
            anchors.centerIn: parent
            spacing: 3 * bar.u
            // Left and Right move along the controls once one has the focus (Tab is the window's own key).
            Keys.onLeftPressed: bar.step(false)
            Keys.onRightPressed: bar.step(true)

            // Chunk 1: the grip.
            MoveGrip {
                id: grab
                objectName: "runToolbarGrip"
                width: 6 * bar.u
                height: 7 * bar.u
                areaWidth: bar.width
                areaHeight: bar.height
                boxWidth: box.width
                boxHeight: box.height
                margin: 4 * bar.u
                place: bar.place
                step: 2 * bar.u
                snapDistance: 3 * bar.u
                onPlaceRequested: place => bar.placeRequested(place)
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Move the run toolbar")
                Accessible.description: qsTr("The arrow keys move it, Shift for bigger steps")
                Text {
                    anchors.centerIn: parent
                    text: "⋮⋮"
                    color: grab.activeFocus ? bar.look.accent : bar.look.label
                    font.pixelSize: 14
                }
            }

            // Chunk 2: batch count, Run and its menu.
            Row {
                spacing: bar.u
                Row {
                    spacing: 0
                    ViewButton {
                        objectName: "runBatchLess"
                        look: bar.look
                        text: "−"
                        tip: qsTr("Fewer runs per Run")
                        enabled: bar.batch > 1
                        onTapped: bar.batchRequested(bar.batch - 1)
                    }
                    Text {
                        objectName: "runBatchCount"
                        width: Math.max(5 * bar.u, implicitWidth + bar.u)
                        height: 7 * bar.u
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: "×" + bar.batch
                        color: bar.batch > 1 ? bar.look.text : bar.look.label
                        font.pixelSize: bar.look.labelSize
                        font.features: { "tnum": 1 }
                        Accessible.name: qsTr("%n runs per Run", "", bar.batch)
                    }
                    ViewButton {
                        objectName: "runBatchMore"
                        look: bar.look
                        text: "+"
                        tip: qsTr("More runs per Run")
                        enabled: bar.batch < bar.maxBatch
                        onTapped: bar.batchRequested(bar.batch + 1)
                    }
                }
                Row {
                    spacing: 0
                    ViewButton {
                        objectName: "runButton"
                        look: bar.look
                        primary: true
                        glyph: "play"
                        text: qsTr("Run")
                        tip: qsTr("Run the workflow")
                        enabled: bar.canRun
                        onTapped: bar.runRequested()
                    }
                    ViewButton {
                        id: menuButton
                        objectName: "runMenuButton"
                        look: bar.look
                        primary: true
                        glyph: "chevron-down"
                        tip: qsTr("More ways to run")
                        onTapped: bar.openMenu()
                    }
                }
            }

            // Chunk 3: Cancel.
            ViewButton {
                objectName: "runCancelButton"
                look: bar.look
                danger: true
                glyph: "x"
                text: qsTr("Cancel")
                tip: qsTr("Cancel the run")
                enabled: bar.canCancel
                onTapped: bar.cancelRequested()
            }

            // Chunk 4: the queue.
            ViewButton {
                objectName: "runQueueButton"
                look: bar.look
                glyph: "layers"
                text: bar.pending > 0 ? String(bar.pending) : ""
                tip: qsTr("Run queue")
                current: bar.queueOpen
                onTapped: bar.queueToggled()
            }
        }
    }
    // The progress of the run going, along the bar's lower edge.
    Rectangle {
        objectName: "runToolbarProgress"
        visible: bar.progress >= 0
        x: box.x + bar.look.popupCorner / 2
        y: box.y + box.height - 3
        width: Math.max(0, (box.width - bar.look.popupCorner) * bar.progress)
        height: 2
        radius: 1
        color: bar.look.accent
    }
}
