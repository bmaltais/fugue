pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue

// The Canvas view's own menu bar (#232 slice B, Blender's editor header menus with Photoshop's content): File, Edit,
// Select, Layer and View, hosted inside the view so the same bar serves the File > Canvas… dialog and an area. The rows are
// the registry's `canvas-view` tree (menus.cpp, CommandRegistry::menu); the view gives each its state (`menuItems`: what
// applies to this canvas, what is checked) and runs the command (CanvasView.qml listens for `canvas-view.*`). A planned
// feature is a greyed row that says which issue builds it. Left and Right move to the next menu, Up and Down inside it, Enter
// runs, Escape closes; a press on another title while one is open moves there, as hovering it does. No Alt+letter here:
// the window's own menu bar owns those letters (the titles keep their `&` for a later per-view binding).
Item {
    id: bar
    objectName: "canvasMenuBar"
    required property CanvasLook look
    // The view that owns this bar: menuItems(index), menuOpen.
    required property var view
    readonly property real u: look.unit
    implicitHeight: 6 * u
    implicitWidth: titles.implicitWidth

    // The top menus' names, read again when the language changes.
    property var tree: App.commands.menu("canvas-view")
    Connections {
        target: App.translator
        function onChanged() { bar.tree = App.commands.menu("canvas-view") }
    }
    property int current: -1
    // The one popup, made the first time (a window that never opens a menu never compiles it).
    property var dropdown: null
    function makeMenu() {
        if (dropdown)
            return
        const component = Qt.createComponent("../menu/CommandMenu.qml")
        dropdown = component.createObject(bar, {objectName: "canvasMenuDropdown",
                                                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent})
        dropdown.stepRequested.connect(delta => bar.openMenu(bar.current + delta, true))
        dropdown.aboutToShow.connect(() => bar.view.menuOpen = true)
        dropdown.aboutToHide.connect(() => { bar.view.menuOpen = false; bar.current = -1 })
    }
    function titleOf(entry) { return entry.title.replace("&", "") }
    // Opens menu `index` under its title; from the keys its first row is current.
    function openMenu(index, byKey) {
        const count = bar.tree.length
        index = (index + count) % count
        bar.makeMenu()
        const menu = bar.dropdown
        if (menu.shown && bar.current === index) {
            if (!byKey)
                menu.close() // a second press on the open title closes it
            return
        }
        bar.view.menuOpen = true // the commands are live from before the popup shows (the pointer leaves the view)
        bar.current = index
        const anchor = titleRepeater.itemAt(index)
        menu.items = bar.view.menuItems(index)
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

    Row {
        id: titles
        spacing: 0
        Repeater {
            id: titleRepeater
            model: bar.tree
            delegate: ViewButton {
                required property int index
                required property var modelData
                objectName: "canvasMenuTitle-" + bar.titleOf(modelData)
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
