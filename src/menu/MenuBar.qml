pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"
import "../kit"
import "../apps"
import "../workflow"

// The window bar (#50, #262; Blender's top bar): the window's own menus, File, Edit, Run, Window and Help, built from
// the command registry's "bar" tree (menus.h), so names, keys and enabled state are the registry's; a view's menus
// are its area header's. After the menus, in the same row, the workflow tabs (workflow/WorkflowTabs.qml), then the
// project chip and Close at the far end.
// Alt+letter opens a menu (the letters after `&` in the titles, none of them bound in 12.4),
// F10 (app.menu-bar) the first one (an auto-hidden bar slides in for both, panels/AutoHide.qml); Up and Down move in a menu, Left and Right to the next
// menu, Enter runs, Escape closes. A press on another title while one is open moves there,
// as does hovering it. When the window is too narrow for the titles, one menu button takes
// their place and opens the same menus as submenus.
Rectangle {
    id: bar
    objectName: "menuBar"

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    // Off while View > Bars and panels has the workflow tabs hidden (#129).
    property bool tabsAllowed: true
    // False while the window has several areas: the App view is not shown then, so the App | Graph switch has nothing to switch.
    property bool appSwitchAllowed: true

    // The top menus; read again when the language changes.
    property var tree: App.commands.menu("bar")
    Connections {
        target: App.translator
        function onChanged() { bar.tree = App.commands.menu("bar") }
    }
    // The open top menu's index, or -1.
    property int current: -1
    readonly property bool narrow: titles.implicitWidth + 8 * u > width
    // The open top menu and the menu button's menu, made the first time one opens so the
    // window opens without them.
    property var dropdown: null
    property var everything: null
    readonly property bool dropdownShown: dropdown !== null && dropdown.shown
    readonly property bool anyShown: dropdownShown || (everything !== null && everything.shown)
    // From the file at run time, so opening the window does not compile Menu's QML.
    function makeMenus() {
        if (dropdown)
            return
        const component = Qt.createComponent("CommandMenu.qml")
        // A press on its own title (its parent) is the title's: it closes it.
        const policy = Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        dropdown = component.createObject(bar, {objectName: "menuDropdown", closePolicy: policy})
        dropdown.stepRequested.connect(delta => bar.openMenu(bar.current + delta, true))
        everything = component.createObject(bar, {objectName: "menuEverything", closePolicy: policy})
    }
    function closeMenus() {
        if (dropdown)
            dropdown.close()
        if (everything)
            everything.close()
    }

    implicitHeight: 7 * u
    color: look.well
    LayoutMirroring.enabled: rightToLeft
    LayoutMirroring.childrenInherit: true

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: bar.look.hairline
    }

    // The bar is wanted in the window: an auto-hide container slides it in (panels/AutoHide.qml).
    signal needed()

    function title(index) { return bar.tree[index].title }
    function mnemonic(index) {
        const t = title(index), at = t.indexOf("&")
        return at >= 0 && at + 1 < t.length ? t.charAt(at + 1).toUpperCase() : ""
    }
    // Opens a top menu under its title (under the menu button when narrow); from the keys,
    // its first row is current so Up and Down start there.
    function openMenu(index, byKey) {
        const count = bar.tree.length
        index = (index + count) % count
        bar.needed()
        bar.makeMenus()
        const dropdown = bar.dropdown
        bar.everything.close()
        if (dropdown.shown && bar.current === index) {
            if (!byKey)
                dropdown.close() // a second press on the open title closes it
            return
        }
        bar.current = index
        const anchor = bar.narrow ? hamburger : titleRepeater.itemAt(index)
        if (dropdown.shown) {
            // Moving along the bar: the same popup, refilled and moved under the next title.
            dropdown.items = App.commands.menu("bar")[index].items
            const at = dropdown.fitted(anchor, 0, anchor.height, anchor.width, anchor.height)
            dropdown.parent = anchor
            dropdown.x = at.x
            dropdown.y = at.y
            dropdown.currentIndex = byKey ? dropdown.firstEnabled() : -1
            // The row that had the keys is gone: the new current row, or the list, takes them.
            const row = dropdown.itemAt(dropdown.currentIndex)
            if (row && row.enabled)
                row.forceActiveFocus(Qt.TabFocusReason)
            else
                dropdown.contentItem.forceActiveFocus(Qt.TabFocusReason)
            return
        }
        dropdown.items = App.commands.menu("bar")[index].items
        dropdown.byKey = byKey
        dropdown.popupFit(anchor, 0, anchor.height, anchor.width, anchor.height)
    }
    function openEverything(byKey) {
        bar.needed()
        bar.makeMenus()
        const everything = bar.everything
        bar.dropdown.close()
        bar.current = -1
        everything.items = App.commands.menu("bar")
        everything.byKey = byKey
        everything.popupFit(hamburger, 0, hamburger.height, hamburger.width, hamburger.height)
    }

    Connections {
        target: App.commands
        function onWindowRequested(id) {
            if (id === "help.about") {
                if (!bar.about)
                    bar.about = aboutComponent.createObject(bar)
                bar.about.open()
                return
            }
            if (id !== "app.menu-bar")
                return
            if (bar.anyShown) {
                bar.closeMenus()
            } else if (bar.narrow) {
                bar.openEverything(true)
            } else {
                bar.openMenu(0, true)
            }
        }
    }

    // Alt+letter for each top menu, the window's own (not the registry's: they are the menu
    // titles' mnemonics, translated with them).
    Repeater {
        model: bar.tree
        delegate: Item {
            id: mnemonicKey
            required property int index
            Shortcut {
                sequence: bar.mnemonic(mnemonicKey.index) !== "" ? "Alt+" + bar.mnemonic(mnemonicKey.index) : ""
                enabled: sequence !== "" // an auto-hidden bar keeps its Alt keys: it slides in (needed)
                onActivated: bar.openMenu(mnemonicKey.index, true)
            }
        }
    }

    // Fugue's mark at the bar's start (the Dusk app icon): the way back to the shelf of apps from any view, `app.show-all`. The
    // first Tab stop of the bar; Enter or Space presses it.
    Rectangle {
        id: mark
        objectName: "fugueMark"
        color: "transparent"
        radius: bar.look.smallCorner + 2
        border.width: activeFocus && FocusInput.keyboard ? 2 : 0
        border.color: bar.look.accent
        anchors.left: parent.left
        anchors.leftMargin: 1.5 * bar.u
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -0.5
        width: 6 * bar.u
        height: 6 * bar.u
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("All apps")
        Accessible.onPressAction: App.commands.trigger("app.show-all")
        Keys.onReturnPressed: App.commands.trigger("app.show-all")
        Keys.onEnterPressed: App.commands.trigger("app.show-all")
        Keys.onSpacePressed: App.commands.trigger("app.show-all")
        Image {
            anchors.fill: parent
            source: App.theme.light ? "qrc:/fugue-light.png" : "qrc:/fugue-dark.png"
            sourceSize: Qt.size(64, 64)
            fillMode: Image.PreserveAspectFit
            mipmap: true
            opacity: markHover.hovered ? 1 : 0.9
        }
        HoverHandler { id: markHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { gesturePolicy: TapHandler.WithinBounds; onTapped: App.commands.trigger("app.show-all") }
        ToolTip.visible: markHover.hovered
        ToolTip.delay: bar.look.tooltipDelay
        ToolTip.text: qsTr("All apps (%1)").arg((App.commands.entry("app.show-all").keys ?? [])[0] ?? "")
    }

    Row {
        id: titles
        visible: !bar.narrow
        height: parent.height - 1
        anchors.left: mark.right
        anchors.leftMargin: 1 * bar.u
        Repeater {
            id: titleRepeater
            model: bar.tree
            delegate: AbstractButton {
                id: titleButton
                required property var modelData
                required property int index
                objectName: "menuTitle-" + index
                readonly property bool open: bar.dropdownShown && bar.current === index
                // No `&` here: the bar binds the Alt keys itself, for the narrow form too.
                text: modelData.title.replace("&", "")
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                height: parent.height
                leftPadding: 2.5 * bar.u
                rightPadding: 2.5 * bar.u
                onPressed: bar.openMenu(index, false)
                onHoveredChanged: if (hovered && bar.dropdownShown && bar.current !== index) bar.openMenu(index, false)
                Accessible.role: Accessible.MenuItem
                background: Rectangle {
                    color: titleButton.open ? bar.look.body
                         : titleButton.hovered ? Qt.tint(bar.look.well, Qt.alpha(bar.look.text, 0.05)) : "transparent"
                }
                contentItem: Text {
                    text: titleButton.text
                    font.pixelSize: bar.look.textSize
                    color: bar.look.text
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    AbstractButton {
        id: hamburger
        objectName: "menuButton"
        visible: bar.narrow
        anchors.left: mark.right
        anchors.leftMargin: 1 * bar.u
        width: 8 * bar.u
        height: parent.height - 1
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        onPressed: bar.anyShown ? bar.closeMenus() : bar.openEverything(false)
        Accessible.name: qsTr("Menu")
        Accessible.role: Accessible.ButtonMenu
        background: Rectangle {
            color: bar.anyShown ? bar.look.body
                 : hamburger.hovered ? Qt.tint(bar.look.well, Qt.alpha(bar.look.text, 0.05)) : "transparent"
        }
        contentItem: Item {
            Column {
                anchors.centerIn: parent
                spacing: 0.75 * bar.u
                Repeater {
                    model: 3
                    Rectangle {
                        width: 4 * bar.u
                        height: Math.max(1, Math.round(bar.u / 2))
                        color: bar.look.text
                    }
                }
            }
        }
    }

    // The workflow tabs: the room between the menus (or the menu button) and the chip, 2u clear of each.
    WorkflowTabs {
        id: tabs
        allowed: bar.tabsAllowed
        anchors.left: bar.narrow ? hamburger.right : titles.right
        anchors.leftMargin: 2 * bar.u
        anchors.right: bar.appSwitch ? bar.appSwitch.left : chipLoader.left
        anchors.rightMargin: 2 * bar.u
        height: parent.height - 1
    }

    // The App | Graph switch (#397): always shown, in every state (only focus mode or a hidden bar removes it), 2u from the project chip,
    // and the tabs end 2u before it. Made once the bar is (the window keeps few items: test_area_window).
    property Item appSwitch: null
    // A bar narrower than 56u has no room for it beside the menu button and the close button (it would cover them): then it waits.
    readonly property bool appSwitchWanted: appSwitchAllowed && width >= 56 * u
    function syncAppSwitch() {
        if (appSwitchWanted && !appSwitch) {
            appSwitch = appSwitchComponent.createObject(bar) as Item
        } else if (!appSwitchWanted && appSwitch) {
            appSwitch.destroy()
            appSwitch = null
        }
    }
    onAppSwitchWantedChanged: syncAppSwitch()
    Component.onCompleted: syncAppSwitch()
    Component {
        id: appSwitchComponent
        AppViewSwitch {
            look: bar.look
            appShown: App.appView !== null && App.appView.shown
            atHome: App.appShelf !== null && App.appShelf.atHome
            plainGraph: App.appView !== null && !App.appView.isApp && !(App.appShelf !== null && App.appShelf.landing)
            anchors.right: chipLoader.left
            anchors.rightMargin: 2 * bar.u
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    // The project chip (spec 1.4.2): which project this window is in, at the bar's far end; a press opens the
    // Projects dialog. Made only inside a project: in the Default there is nothing to tell (File > Projects… is the way in).
    // A KitChip like the bar's other buttons, with no accent (D7: the accent is the primary's and the focus ring's).
    Loader {
        id: chipLoader
        objectName: "projectChipLoader"
        active: App.projects !== null && App.projects.indicatorShown
        anchors.right: closeButton.left
        anchors.rightMargin: 2 * bar.u
        anchors.verticalCenter: parent.verticalCenter
        sourceComponent: KitChip {
            objectName: "projectChip"
            look: bar.look
            pill: true
            text: App.projects.indicator
            maxTextWidth: 25 * bar.u
            tip: qsTr("Project: %1. Switch or manage projects (File > Projects…)").arg(text)
            onTapped: App.commands.trigger("file.projects")
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Project: %1").arg(text)
        }
    }

    // Close Fugue (#218): the window has no title bar on a tiling desktop, so the bar's far corner
    // quits. It runs app.quit, so the question about unsaved content applies. It reaches the window's
    // edge and the bar's full height (a corner is the easiest target to hit, Fitts), and the project
    // chip sits 2u away from it, so a slip next to either does not press the other.
    AbstractButton {
        id: closeButton
        objectName: "closeApp"
        // Read as the pointer arrives, so a remapped key shows.
        property var entry: ({})
        readonly property string key: entry.keys && entry.keys.length > 0
            ? entry.keys[0].replace("Return", "Enter").replace("Escape", "Esc") : ""
        anchors.right: parent.right
        width: 10 * bar.u
        height: parent.height - 1
        focusPolicy: Qt.TabFocus
        hoverEnabled: true
        onHoveredChanged: if (hovered) entry = App.commands.entry("app.quit")
        onClicked: App.commands.trigger("app.quit")
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Close Fugue")
        Accessible.onPressAction: clicked()
        ToolTip.visible: hovered
        ToolTip.delay: bar.look.tooltipDelay
        ToolTip.text: key !== "" ? qsTr("Close Fugue (%1)").arg(key) : qsTr("Close Fugue")
        background: Rectangle {
            color: closeButton.pressed ? bar.look.dangerHover : closeButton.hovered ? bar.look.dangerFill : "transparent"
            // The 2px accent ring inside it, as every other button's focus (ViewButton, canvas-look 11).
            Rectangle {
                anchors { fill: parent; margins: 1 }
                visible: closeButton.visualFocus
                color: "transparent"
                border { width: 2; color: bar.look.accent }
                radius: bar.look.smallCorner
            }
        }
        contentItem: Glyph {
            look: bar.look
            name: "x"
            font.pixelSize: 16
            color: closeButton.hovered || closeButton.visualFocus ? bar.look.error : bar.look.label
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    // Help > About Fugue: the name, version and what it is; Esc or a click outside closes it.
    property var about: null
    Component {
        id: aboutComponent
        Popup {
            id: aboutPopup
            objectName: "about"
            parent: Overlay.overlay
            anchors.centerIn: parent
            modal: true
            focus: true
            padding: 0
            width: 80 * bar.u
            Overlay.modal: Rectangle { color: bar.look.scrim }
            topPadding: aboutHero.height
            background: Item {
                PopupSurface { anchors.fill: parent; look: bar.look; tone: "dusk" }
                KitHero {
                    id: aboutHero
                    objectName: "aboutHero"
                    look: bar.look
                    tone: "dusk"
                    title: qsTr("Fugue %1").arg(Qt.application.version)
                    subtitle: qsTr("Node-based workflows for local AI on Omarchy.")
                    LayoutMirroring.enabled: Application.layoutDirection === Qt.RightToLeft
                    LayoutMirroring.childrenInherit: true
                }
            }
            onOpened: aboutClose.forceActiveFocus(Qt.OtherFocusReason)
            contentItem: Item {
                implicitHeight: 4 * bar.u + 7 * bar.u
                LayoutMirroring.enabled: Application.layoutDirection === Qt.RightToLeft
                LayoutMirroring.childrenInherit: true
                KitChip {
                    id: aboutClose
                    objectName: "aboutClose"
                    anchors { right: parent.right; rightMargin: 5 * bar.u; bottom: parent.bottom; bottomMargin: 4 * bar.u }
                    look: bar.look
                    text: qsTr("Close")
                    bordered: true
                    onTapped: aboutPopup.close()
                }
                Text {
                    anchors { left: parent.left; leftMargin: 5 * bar.u; verticalCenter: aboutClose.verticalCenter }
                    text: qsTr("MIT license.")
                    color: bar.look.label
                    font.pixelSize: bar.look.labelSize
                }
            }
        }
    }
}
