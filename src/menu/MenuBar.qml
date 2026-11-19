pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"

// The menu bar (#50): File, Edit, View, Node, Run and Help, built from the command
// registry's "bar" tree (menus.h), so names, keys and enabled state are the registry's.
// Alt+letter opens a menu (the letters after `&` in the titles, none of them bound in 12.4),
// F10 (app.menu-bar) the first one; Up and Down move in a menu, Left and Right to the next
// menu, Enter runs, Escape closes. A press on another title while one is open moves there,
// as does hovering it. When the window is too narrow for the titles, one menu button takes
// their place and opens the same menus as submenus.
Rectangle {
    id: bar
    objectName: "menuBar"

    readonly property var look: App.theme.look
    readonly property real u: look.unit
    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft

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
            dropdown.parent = anchor
            dropdown.x = 0
            dropdown.y = anchor.height
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
        dropdown.popup(anchor, 0, anchor.height)
    }
    function openEverything(byKey) {
        bar.makeMenus()
        const everything = bar.everything
        bar.dropdown.close()
        bar.current = -1
        everything.items = App.commands.menu("bar")
        everything.byKey = byKey
        everything.popup(hamburger, 0, hamburger.height)
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
                enabled: sequence !== ""
                onActivated: bar.openMenu(mnemonicKey.index, true)
            }
        }
    }

    Row {
        id: titles
        visible: !bar.narrow
        height: parent.height - 1
        anchors.left: parent.left
        anchors.leftMargin: 1.5 * bar.u
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
        anchors.left: parent.left
        anchors.leftMargin: 1.5 * bar.u
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

    // Help > About Fugue: the name, version and what it is; Esc or a click outside closes it.
    property var about: null
    Component {
        id: aboutComponent
        Popup {
            objectName: "about"
            parent: Overlay.overlay
            anchors.centerIn: parent
            modal: true
            focus: true
            padding: 6 * bar.u
            Overlay.modal: Rectangle { color: bar.look.scrim }
            background: PopupSurface { look: bar.look }
            contentItem: Column {
                spacing: 2 * bar.u
                Text {
                    text: qsTr("Fugue %1").arg(Qt.application.version)
                    font.pixelSize: bar.look.overlayTitleSize
                    font.weight: Font.DemiBold
                    color: bar.look.text
                }
                Text {
                    text: qsTr("Node-based workflows for local AI on Omarchy.")
                    color: bar.look.label
                    font.pixelSize: bar.look.textSize
                }
                Text {
                    text: qsTr("MIT license.")
                    color: bar.look.label
                    font.pixelSize: bar.look.labelSize
                }
            }
        }
    }
}
