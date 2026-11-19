pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "placement.js" as Placement

// A menu built from a menu tree of the command registry (menus.h, CommandRegistry::menu): a
// MenuItem per command with its name, keys and state as the registry gives them now, a
// separator, or a submenu (the Colors one shows a swatch per theme role). The owner sets
// `items` just before it opens, so the state is fresh each time. Triggering an item runs
// its command through the registry, with `target` (what a context menu was opened on).
// Styled per docs/canvas-look.md: the popup surface (level 3), rows on the search's
// highlight, keys in `label`; a command not built yet at half opacity, as in the overlay.
Menu {
    id: menu

    property var items: []
    property var target: ({})
    property bool sub: false // a submenu (Colors)
    // True while one of its submenus is open (Left and Right are the submenu's then).
    property bool subOpen: false
    // Left or Right on one of its rows: the menu bar moves to the neighbouring menu.
    signal stepRequested(int delta)

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft

    // Not modal, so a press on another menu title or on the canvas both closes it and lands
    // there; while it is open the window's shortcuts sleep (CommandRegistry::menuOpen), so
    // bare letters reach its rows' mnemonics.
    modal: false
    // Drawn in the window, on the theme's surface, never as a native or separate window.
    popupType: Popup.Item
    // Opened from the keys: its first row is current, so Up and Down start there.
    property bool byKey: false
    onOpened: if (byKey) currentIndex = firstEnabled()
    // The first row that can run, else the first row.
    function firstEnabled() {
        for (let i = 0; i < count; ++i) {
            const row = itemAt(i)
            if (row && row.enabled && !(row instanceof MenuSeparator))
                return i
        }
        return count > 0 ? 0 : -1
    }
    // The current row opens a submenu.
    readonly property bool onSubMenuRow: currentIndex >= 0 && !!(itemAt(currentIndex) as MenuItem)?.subMenu
    onClosed: byKey = false
    // Open, or opening; false as soon as it starts to close (`visible` stays true through the
    // closing fade, `opened` is false through the opening one).
    property bool shown: false
    onAboutToShow: { shown = true; App.commands.menuShown(true) }
    onAboutToHide: { shown = false; App.commands.menuShown(false) }
    Component.onDestruction: if (shown) App.commands.menuShown(false)
    padding: 1.5 * u
    // Wholly inside the window: no taller than it (the rows scroll then), and opened through
    // popupFit, which flips or shifts it (placement.js). Qt's own clamp is the backstop.
    readonly property Item overlay: Overlay.overlay
    margins: Placement.margin
    height: Math.min(implicitHeight, overlay ? Placement.maxExtent(overlay.height) : implicitHeight)
    // Opens at (x, y) of `anchor`; `gapX` and `gapY` are what the anchor takes beside that point.
    function popupFit(anchor, x, y, gapX, gapY) {
        const p = fitted(anchor, x, y, gapX, gapY)
        popup(anchor, p.x, p.y)
    }
    // Where it fits, in `anchor`'s coordinates; the rows must be in already.
    function fitted(anchor, x, y, gapX, gapY) {
        (contentItem as ListView).forceLayout()
        return Placement.fit(anchor, overlay, x, y, implicitWidth, implicitHeight, gapX, gapY)
    }
    // As wide as its widest row wants, so names and keys never elide.
    property real widest: 0
    implicitWidth: Math.max(56 * u, widest + leftPadding + rightPadding)
    background: PopupSurface { look: menu.look }

    delegate: CommandMenuItem {}

    // Material's list, holding the keys when no row does (a menu whose rows cannot run yet),
    // so Left and Right reach the menu bar from any row: rows pass them on to it.
    contentItem: ListView {
        implicitHeight: contentHeight
        model: menu.contentModel
        interactive: Window.window ? contentHeight + menu.topPadding + menu.bottomPadding > menu.height : false
        clip: true
        focus: true
        keyNavigationEnabled: false // Up and Down are the menu's
        currentIndex: menu.currentIndex
        // Up and Down keep the current row in view when the menu is capped and scrolls.
        onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
        ScrollIndicator.vertical: ScrollIndicator {}
        Keys.onPressed: event => {
            if (menu.sub || menu.onSubMenuRow || (event.key !== Qt.Key_Left && event.key !== Qt.Key_Right))
                return
            menu.stepRequested((event.key === Qt.Key_Right) !== menu.rightToLeft ? 1 : -1)
            event.accepted = true
        }
    }

    property var made: []
    onItemsChanged: rebuild()
    function rebuild() {
        while (menu.count > 0)
            menu.takeItem(0)
        for (const old of menu.made)
            old.destroy()
        const list = []
        for (const entry of menu.items) {
            if (entry.separator) {
                const separator = separatorComponent.createObject(menu.contentItem)
                menu.addItem(separator)
                list.push(separator)
            } else if (entry.items !== undefined) {
                // Made from the file at run time: a type cannot hold itself.
                if (!menu.subMenuComponent)
                    menu.subMenuComponent = Qt.createComponent("CommandMenu.qml")
                const sub = menu.subMenuComponent.createObject(menu, {title: entry.title, items: entry.items,
                                                                      target: menu.target, sub: true,
                                                                      cascade: true, overlap: menu.u}) as Menu
                sub.aboutToShow.connect(() => menu.subOpen = true)
                sub.aboutToHide.connect(() => menu.subOpen = false)
                menu.addMenu(sub)
                list.push(sub)
            } else {
                const item = itemComponent.createObject(menu.contentItem, {entry: entry, target: Qt.binding(() => menu.target)})
                menu.addItem(item)
                list.push(item)
            }
        }
        menu.made = list
        let widest = 0
        for (let i = 0; i < menu.count; ++i)
            widest = Math.max(widest, menu.itemAt(i) ? menu.itemAt(i).implicitWidth : 0)
        menu.widest = widest
    }

    Component {
        id: itemComponent
        CommandMenuItem {}
    }
    Component {
        id: separatorComponent
        MenuSeparator {
            padding: 0
            topPadding: menu.u
            bottomPadding: menu.u
            contentItem: Rectangle {
                implicitHeight: 1
                color: menu.look.hairline
            }
        }
    }
    property Component subMenuComponent: null
}
