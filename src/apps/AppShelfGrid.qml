pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// The apps as a grid of cards (#388), one component for the home shelf and the File > Apps… dialog. It reads AppShelf.apps and
// says `chosen(id)`; the owner decides what a choice closes. Arrow keys move over the cards and Enter or Space opens the
// focused one. With no apps it says so and points to the Templates window.
Item {
    id: grid

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property var apps: App.appShelf ? App.appShelf.apps : []
    signal chosen(string id)

    // Focus on the cards, on the card of the app now open when the shelf lists it (a template's folder is its id), else the first.
    function focusCards() {
        const file = App.appView ? App.appView.file : ""
        let at = 0
        for (let i = 0; file !== "" && i < apps.length; ++i)
            if (file.endsWith("/" + apps[i].id + "/workflow.fugue")) {
                at = i
                break
            }
        cards.currentIndex = at
        cards.positionViewAtIndex(at, GridView.Contain)
        cards.forceActiveFocus()
    }

    GridView {
        id: cards
        objectName: "appShelfGrid"
        anchors.fill: parent
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: FugueScrollBar { look: grid.look }
        model: grid.apps
        keyNavigationEnabled: true
        activeFocusOnTab: true
        readonly property int room: width - (ScrollBar.vertical as FugueScrollBar).breadth
        readonly property int columns: Math.max(1, Math.floor(room / (56 * grid.u)))
        cellWidth: Math.floor(room / columns)
        FontMetrics { id: titleMetrics; font.pixelSize: grid.look.textSize; font.weight: Font.DemiBold }
        FontMetrics { id: bodyMetrics; font.pixelSize: grid.look.labelSize }
        cellHeight: Math.ceil((cellWidth - 8 * grid.u) * 10 / 16 + 4 * grid.u + 3 * grid.u + 2 * titleMetrics.height
                              + 3 * bodyMetrics.height + bodyMetrics.height + 2 * grid.u)
        Keys.onReturnPressed: if (currentIndex >= 0) grid.chosen(grid.apps[currentIndex].id)
        Keys.onEnterPressed: if (currentIndex >= 0) grid.chosen(grid.apps[currentIndex].id)
        Keys.onSpacePressed: if (currentIndex >= 0) grid.chosen(grid.apps[currentIndex].id)

        delegate: Item {
            id: cell
            required property var modelData
            required property int index
            width: cards.cellWidth
            height: cards.cellHeight
            AppCard {
                objectName: "appCard-" + cell.modelData.id
                anchors { fill: parent; margins: grid.u }
                look: grid.look
                entry: cell.modelData
                current: cards.activeFocus && cards.currentIndex === cell.index
                onChosen: {
                    cards.currentIndex = cell.index
                    grid.chosen(cell.modelData.id)
                }
            }
        }
    }

    ColumnLayout {
        objectName: "appShelfEmpty"
        anchors.centerIn: parent
        width: parent.width - 16 * grid.u
        visible: grid.apps.length === 0
        spacing: 2 * grid.u
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: qsTr("No apps here yet. Templates that are apps will show up here.")
            color: grid.look.label
            font.pixelSize: grid.look.textSize
        }
        ThemedButton {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Browse templates…")
            onClicked: App.commands.trigger("file.templates")
        }
    }
}
