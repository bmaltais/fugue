pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "../kit"

// The Needs panel of the App view (#469, spec 12.17): while the app is not ready, or a run failed for a missing model or pack, a
// quiet persistent block at the head of the input panel (not a toast) lists each thing it still needs with its size and licence,
// Download for one, Download all for the lot (the Get ready path: the sheet's plan, run once), and for a builder "Show in graph" at
// the node that holds it. It is a KitGroup in the panel under a header line in Moss (the tone of Get ready and downloads) and
// clears by itself when the app is ready. All the logic is NeedsModel's (apps/needs_model.h); this draws it.
//
// Keys: Tab reaches Download all first (the header line), then each row's buttons; Enter or Space press the focused button.
Item {
    id: panel
    objectName: "appNeeds"
    property CanvasLook look: App.theme.look
    required property NeedsModel needs
    property bool standalone: false // the app's own window has no graph to show a node in
    readonly property real u: look.unit
    readonly property string kitTone: "moss" // the controls inside read the window's tone (KitTone.js)
    readonly property color toneColor: look.tone("moss").glass
    readonly property bool jump: needs.canShowInGraph && !standalone
    property real maxHeight: 40 * u

    visible: !needs.ready
    implicitHeight: visible ? Math.min(column.implicitHeight + 2 * u, maxHeight) : 0
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("What this app needs")

    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight + 2 * panel.u
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: FugueScrollBar { look: panel.look }
        Column {
            id: column
            x: 0
            width: flick.width - (flick.ScrollBar.vertical as FugueScrollBar).inset
            spacing: 2 * panel.u
            // The header line: a bar of the tone, what it needs, and Download all at the end.
            Item {
                width: parent.width
                height: Math.max(7 * panel.u, headline.contentHeight)
                Rectangle {
                    id: bar
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    width: 0.75 * panel.u
                    height: parent.height - 2 * panel.u
                    radius: width / 2
                    color: panel.toneColor
                }
                Text {
                    id: headline
                    objectName: "appNeedsHeadline"
                    anchors { left: bar.right; leftMargin: 2 * panel.u; right: all.left; rightMargin: 2 * panel.u; verticalCenter: parent.verticalCenter }
                    text: panel.needs.headline
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    color: panel.look.text
                    font.pixelSize: Math.round(panel.look.textSize * 7 / 6)
                    font.weight: Font.Medium
                }
                KitChip {
                    id: all
                    objectName: "appNeedsAll"
                    look: panel.look
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    glyph: "download"
                    text: panel.needs.startsAtOnce ? qsTr("Download all") : qsTr("Get ready…")
                    toned: true
                    current: true
                    visible: panel.needs.items.some(i => i.kind !== "problem")
                    onTapped: panel.needs.downloadAll()
                }
            }
            KitGroup {
                look: panel.look
                Repeater {
                    model: panel.needs.items
                    delegate: KitRow {
                        id: row
                        required property var modelData
                        required property int index
                        objectName: "appNeed-" + modelData.key
                        look: panel.look
                        label: modelData.name
                        hint: [modelData.size, modelData.licence, modelData.detail, modelData.use && modelData.use.text ? modelData.use.text : ""].filter(t => t !== "").join(" · ")
                        KitChip {
                            objectName: "appNeedShow-" + row.modelData.key
                            look: panel.look
                            glyph: "workflow"
                            tip: qsTr("Show in graph: the node that needs it")
                            round: true
                            visible: panel.jump && row.modelData.node !== ""
                            onTapped: panel.needs.showInGraph(row.index)
                        }
                        // The same model is on disk in another file (#605): the choice beside the download; it leaves this list.
                        KitChip {
                            objectName: "appNeedUse-" + row.modelData.key
                            look: panel.look
                            text: qsTr("Use what you have")
                            tip: row.modelData.use && row.modelData.use.text ? row.modelData.use.text : ""
                            visible: row.modelData.kind === "model" && !!(row.modelData.use && row.modelData.use.text)
                            onTapped: panel.needs.useOnDisk(row.index)
                        }
                        KitChip {
                            objectName: "appNeedGet-" + row.modelData.key
                            look: panel.look
                            text: row.modelData.action
                            visible: row.modelData.kind !== "problem"
                            onTapped: panel.needs.download(row.index)
                        }
                    }
                }
            }
        }
    }
}
