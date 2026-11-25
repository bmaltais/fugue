pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../canvas/widgets"
import "../kit"

// The apps as a grid of kit cards (#388, #463, #470), one component for the home shelf and the File > Apps… dialog. It reads
// AppShelf.shown (the apps the search leaves, pinned ones first) and says `chosen(id)`; the owner decides what a choice closes.
// Keys: the arrows move over the cards (Up from the first row goes back to the search), Enter or Space opens the focused one,
// F2 renames it (one of the person's own), Ctrl+D duplicates it, Ctrl+B pins it, and Menu or Shift+F10 opens its menu. With no
// apps it says so and points to the Templates window; with a search that matches none it says so.
Item {
    id: grid

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property var apps: App.appShelf ? App.appShelf.shown : []
    readonly property bool searching: App.appShelf ? App.appShelf.query.trim() !== "" : false
    // The grid's own breathing room: the cards' gap is half on each side, so the cards line up with the header's text.
    readonly property real gap: look.params.areaGap * u
    property string renamingId: ""
    readonly property Item cardsItem: cards
    property Item tabTarget: null // where Tab and Shift+Tab go from the cards (the owner's order)
    property Item backtabTarget: null
    // The app the keyboard is on, kept by id: pinning, renaming or a refresh makes the list again, and the place follows the app.
    property string currentId: ""
    signal chosen(string id)
    signal searchRequested // Up from the first row

    // Focus on the cards, on the card of the app now open when the shelf lists it (a template's folder is its id), else the first.
    function focusCards() {
        const file = App.appView ? App.appView.file : ""
        let at = 0
        for (let i = 0; file !== "" && i < apps.length; ++i)
            if (file.endsWith("/" + apps[i].id + "/workflow.fugue")) {
                at = i
                break
            }
        focusAt(at)
    }
    function focusAt(index) {
        cards.currentIndex = Math.max(0, Math.min(index, apps.length - 1))
        syncCurrent()
        cards.positionViewAtIndex(cards.currentIndex, GridView.Contain)
        cards.forceActiveFocus()
    }
    function syncCurrent() {
        const entry = entryAt(cards.currentIndex)
        if (entry)
            currentId = entry.id
    }
    function restoreCurrent() {
        const at = apps.findIndex(a => a.id === currentId)
        if (at >= 0 && cards.currentIndex !== at)
            cards.currentIndex = at
    }
    onAppsChanged: Qt.callLater(restoreCurrent)
    function entryAt(index) { return index >= 0 && index < apps.length ? apps[index] : null }
    function toggleBookmark(entry) {
        if (entry)
            App.appShelf.toggleBookmark(entry.id)
    }
    function duplicate(entry) {
        if (entry)
            App.appShelf.duplicate(entry.id) // opens the copy in the App view: the shelf's `chosen` follows
    }
    function startRename(entry) {
        if (entry && entry.canRename)
            renamingId = entry.id
    }
    function commitRename(entry, title) {
        renamingId = ""
        if (title.trim() !== "" && title.trim() !== entry.title)
            App.appShelf.rename(entry.id, title)
        cards.forceActiveFocus()
    }
    function showMenu(entry, anchor) {
        menu.entry = entry
        const at = anchor.mapToItem(Overlay.overlay, 0, anchor.height)
        menu.wantX = at.x
        menu.wantY = at.y
        menu.anchorHeight = anchor.height
        menu.open()
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
        KeyNavigation.tab: grid.tabTarget
        KeyNavigation.backtab: grid.backtabTarget
        readonly property int room: width - (ScrollBar.vertical as FugueScrollBar).breadth
        readonly property int columns: Math.max(1, Math.floor(room / (56 * grid.u)))
        cellWidth: Math.floor(room / columns)
        readonly property real pad: grid.look.params.rowPadY * grid.u
        readonly property real margin: grid.gap / 2
        FontMetrics { id: titleMetrics; font.pixelSize: grid.look.textSize; font.weight: Font.DemiBold }
        FontMetrics { id: bodyMetrics; font.pixelSize: grid.look.labelSize }
        // The picture (10:16), a title of up to two lines, three of summary, the ready line, and the padding and gaps between.
        cellHeight: Math.ceil((cellWidth - 2 * margin - 2 * pad) * 10 / 16 + 2 * pad + 2 * margin + 4 * grid.u + 2 * titleMetrics.height
                              + 3 * bodyMetrics.height + bodyMetrics.height)
        Keys.onReturnPressed: if (currentIndex >= 0) grid.chosen(grid.apps[currentIndex].id)
        Keys.onEnterPressed: if (currentIndex >= 0) grid.chosen(grid.apps[currentIndex].id)
        Keys.onSpacePressed: if (currentIndex >= 0) grid.chosen(grid.apps[currentIndex].id)
        Keys.onUpPressed: event => {
            if (currentIndex < columns)
                grid.searchRequested()
            else
                event.accepted = false
        }
        // The cards' own keys win over the window's shortcuts for the same chords (Ctrl+B, Ctrl+D and F2 are canvas commands).
        Keys.onShortcutOverride: event => {
            const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
            event.accepted = event.key === Qt.Key_F2 || (ctrl && (event.key === Qt.Key_D || event.key === Qt.Key_B))
        }
        Keys.onPressed: event => {
            const entry = grid.entryAt(currentIndex)
            if (!entry)
                return
            grid.currentId = entry.id
            if (event.key === Qt.Key_F2) {
                grid.startRename(entry)
            } else if (event.key === Qt.Key_D && (event.modifiers & Qt.ControlModifier)) {
                grid.duplicate(entry)
            } else if (event.key === Qt.Key_B && (event.modifiers & Qt.ControlModifier)) {
                grid.toggleBookmark(entry)
            } else if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                const item = cards.itemAtIndex(currentIndex)
                if (item)
                    grid.showMenu(entry, item)
            } else {
                Qt.callLater(grid.syncCurrent) // an arrow moved the place
                return
            }
            event.accepted = true
        }

        delegate: Item {
            id: cell
            required property var modelData
            required property int index
            width: cards.cellWidth
            height: cards.cellHeight
            // On screen: the cell reaches into the view (the clip cuts the rest).
            readonly property bool inView: grid.visible && y + height > cards.contentY && y < cards.contentY + cards.height
            AppCard {
                objectName: "appCard-" + cell.modelData.id
                anchors { fill: parent; margins: cards.margin }
                look: grid.look
                entry: cell.modelData
                inView: cell.inView
                current: cards.activeFocus && cards.currentIndex === cell.index
                renaming: grid.renamingId === cell.modelData.id
                onChosen: {
                    cards.currentIndex = cell.index
                    grid.syncCurrent()
                    grid.chosen(cell.modelData.id)
                }
                onBookmarkToggled: grid.toggleBookmark(cell.modelData)
                onMenuRequested: anchor => {
                    cards.currentIndex = cell.index
                    grid.syncCurrent()
                    grid.showMenu(cell.modelData, anchor)
                }
                onRenamed: title => grid.commitRename(cell.modelData, title)
                onRenameCancelled: {
                    grid.renamingId = ""
                    cards.forceActiveFocus()
                }
            }
        }
    }

    // A card's menu: pin, duplicate, rename (the person's own apps only).
    WidgetMenu {
        id: menu
        objectName: "appCardMenu"
        look: grid.look
        parent: Overlay.overlay
        property var entry: null
        width: 30 * grid.u
        entries: menu.entry === null ? [] : [
            {text: menu.entry.bookmarked ? qsTr("Unpin") : qsTr("Pin to the top"), value: "bookmark"},
            {text: qsTr("Duplicate"), value: "duplicate", detail: qsTr("Ctrl+D")},
            {text: qsTr("Rename"), value: "rename", detail: qsTr("F2"), enabled: menu.entry.canRename}
        ]
        onPicked: v => {
            const entry = menu.entry
            if (v === "bookmark")
                grid.toggleBookmark(entry)
            else if (v === "duplicate")
                grid.duplicate(entry)
            else if (v === "rename")
                grid.startRename(entry)
            if (v !== "rename")
                cards.forceActiveFocus()
        }
    }

    ColumnLayout {
        objectName: "appShelfEmpty"
        anchors.centerIn: parent
        width: parent.width - 16 * grid.u
        visible: grid.apps.length === 0
        spacing: 2 * grid.u
        Text {
            objectName: "appShelfEmptyText"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: grid.searching ? qsTr("No app matches “%1”.").arg(App.appShelf.query.trim())
                                 : qsTr("No apps here yet. Templates that are apps will show up here.")
            color: grid.look.label
            font.pixelSize: grid.look.textSize
        }
        KitChip {
            Layout.alignment: Qt.AlignHCenter
            visible: !grid.searching
            look: grid.look
            text: qsTr("Browse templates…")
            bordered: true
            onTapped: App.commands.trigger("file.templates")
        }
    }
}
