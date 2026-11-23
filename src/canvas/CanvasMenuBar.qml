pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue

// A view's own menus (#232 slice B, #262; Blender's editor header menus). The Canvas view's (Canvas, Edit, Select, Layer and
// View), hosted inside the view so the same bar serves the File > Canvas… dialog and an area: the rows are the registry's
// `canvas-view` tree (menus.cpp, CommandRegistry::menu); the view gives each its state (`menuItems`: what applies to this
// canvas, what is checked) and runs the command (CanvasView.qml listens for `canvas-view.*`). The Graph's (View, Select, Add,
// Node), in its area's header: `treeName` "graph", no `view`; the registry's rows carry their own state. Where the titles do
// not fit in `maxWidth`, one Menu button opens the same menus as submenus. A planned
// feature is a greyed row that says which issue builds it. Left and Right move to the next menu, Up and Down inside it, Enter
// runs, Escape closes; a press on another title while one is open moves there, as hovering it does. No Alt+letter here:
// the window's own menu bar owns those letters (the titles keep their `&` for a later per-view binding).
Item {
    id: bar
    objectName: bar.idPrefix + "MenuBar"
    required property CanvasLook look
    // The view that owns this bar: menuItems(index), menuOpen. None: the registry's rows as they are (the Graph's).
    property var view: null
    property string treeName: "canvas-view"
    // The object names' start (canvasMenuTitle-View, graphMenuTitle-View): two bars of one window never share a name.
    property string idPrefix: "canvas"
    // The room the bar has (-1: any); the titles fold into one Menu button when they need more.
    property real maxWidth: -1
    readonly property real u: look.unit
    readonly property bool narrow: maxWidth >= 0 && titles.implicitWidth > maxWidth
    implicitHeight: 6 * u
    implicitWidth: narrow ? menuButton.width : titles.implicitWidth

    // The top menus' names, read again when the language changes.
    property var tree: App.commands.menu(bar.treeName)
    Connections {
        target: App.translator
        function onChanged() { bar.tree = App.commands.menu(bar.treeName) }
    }
    // The rows of menu `index`: the view's (with its state), or the registry's.
    function itemsOf(index) { return bar.view ? bar.view.menuItems(index) : App.commands.menu(bar.treeName)[index].items }
    function setOpen(open) { if (bar.view) bar.view.menuOpen = open }
    property int current: -1
    // The one popup, made the first time (a window that never opens a menu never compiles it).
    property var dropdown: null
    function makeMenu() {
        if (dropdown)
            return
        const component = Qt.createComponent("../menu/CommandMenu.qml")
        dropdown = component.createObject(bar, {objectName: bar.idPrefix + "MenuDropdown",
                                                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent})
        dropdown.stepRequested.connect(delta => bar.openMenu(bar.current + delta, true))
        dropdown.aboutToShow.connect(() => bar.setOpen(true))
        dropdown.aboutToHide.connect(() => { bar.setOpen(false); bar.current = -1 })
    }
    function titleOf(entry) { return entry.title.replace("&", "") }
    // Opens menu `index` under its title; from the keys its first row is current.
    function openMenu(index, byKey) {
        const count = bar.tree.length
        index = (index + count) % count
        bar.makeMenu()
        if (bar.everything)
            bar.everything.close()
        const menu = bar.dropdown
        if (menu.shown && bar.current === index) {
            if (!byKey)
                menu.close() // a second press on the open title closes it
            return
        }
        bar.setOpen(true) // the commands are live from before the popup shows (the pointer leaves the view)
        bar.current = index
        const anchor = bar.narrow ? menuButton : titleRepeater.itemAt(index)
        menu.items = bar.itemsOf(index)
        if (menu.shown) {
            const at = menu.fitted(anchor, 0, anchor.height, anchor.width, anchor.height)
            menu.parent = anchor
            menu.x = at.x
            menu.y = at.y
            menu.currentIndex = byKey ? menu.firstEnabled() : -1
            return
        }
        menu.byKey = byKey
        menu.parent = anchor
        menu.popupFit(anchor, 0, anchor.height, anchor.width, anchor.height)
    }
    // For a test and for a key of the host: the menus by name.
    function openByName(name) {
        for (let i = 0; i < bar.tree.length; ++i)
            if (bar.titleOf(bar.tree[i]) === name) {
                bar.openMenu(i, false)
                return true
            }
        return false
    }
    function closeMenu() {
        if (dropdown)
            dropdown.close()
    }

    // Too narrow for the titles: one button, the menus as its submenus.
    property var everything: null
    function openEverything() {
        if (everything && everything.shown) {
            everything.close()
            return
        }
        if (!everything) {
            const component = Qt.createComponent("../menu/CommandMenu.qml")
            everything = component.createObject(bar, {objectName: bar.idPrefix + "MenuEverything",
                                                      closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent})
            everything.aboutToShow.connect(() => bar.setOpen(true))
            everything.aboutToHide.connect(() => bar.setOpen(false))
        }
        const all = []
        for (let i = 0; i < bar.tree.length; ++i)
            all.push({title: bar.tree[i].title, items: bar.itemsOf(i)})
        everything.items = all
        everything.popupFit(menuButton, 0, menuButton.height, menuButton.width, menuButton.height)
    }
    ViewButton {
        id: menuButton
        objectName: bar.idPrefix + "MenuButton"
        visible: bar.narrow
        look: bar.look
        flat: true
        compact: true
        accented: false
        text: qsTr("Menu")
        trailingGlyph: "chevron-down"
        Accessible.role: Accessible.ButtonMenu
        onTapped: bar.openEverything()
    }
    Row {
        id: titles
        spacing: 0
        visible: !bar.narrow
        Repeater {
            id: titleRepeater
            model: bar.tree
            delegate: ViewButton {
                required property int index
                required property var modelData
                objectName: bar.idPrefix + "MenuTitle-" + bar.titleOf(modelData)
                look: bar.look
                flat: true
                compact: true
                accented: false
                current: bar.current === index
                text: bar.titleOf(modelData)
                Accessible.role: Accessible.MenuItem
                onTapped: bar.openMenu(index, false)
                // Moving along the bar while a menu is open.
                onHoveredChanged: if (hovered && bar.dropdown && bar.dropdown.shown && bar.current !== index) bar.openMenu(index, false)
            }
        }
    }
}
