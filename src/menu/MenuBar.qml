pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
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

    readonly property var look: theme.look
    readonly property real u: look.unit
    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft

    // The top menus; read again when the language changes.
    property var tree: commands.menu("bar")
    Connections {
        target: translator
        function onChanged() { bar.tree = commands.menu("bar") }
    }
    // The open top menu's index, or -1.
    property int current: -1
    readonly property bool narrow: titles.implicitWidth + 8 * u > width
    readonly property alias dropdown: dropdown
    readonly property alias hamburgerMenu: everything

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
        everything.close()
        if (dropdown.shown && bar.current === index) {
            if (!byKey)
                dropdown.close() // a second press on the open title closes it
            return
        }
        bar.current = index
        const anchor = bar.narrow ? hamburger : titleRepeater.itemAt(index)
        if (dropdown.shown) {
            // Moving along the bar: the same popup, refilled and moved under the next title.
            dropdown.items = commands.menu("bar")[index].items
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
        dropdown.items = commands.menu("bar")[index].items
        dropdown.byKey = byKey
        dropdown.popup(anchor, 0, anchor.height)
    }
    function openEverything(byKey) {
        dropdown.close()
        bar.current = -1
        everything.items = commands.menu("bar")
        everything.byKey = byKey
        everything.popup(hamburger, 0, hamburger.height)
    }

    Connections {
        target: commands
        function onWindowRequested(id) {
            if (id !== "app.menu-bar")
                return
            if (dropdown.shown || everything.shown) {
                dropdown.close()
                everything.close()
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
                readonly property bool open: dropdown.shown && bar.current === index
                // No `&` here: the bar binds the Alt keys itself, for the narrow form too.
                text: modelData.title.replace("&", "")
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                height: parent.height
                leftPadding: 2.5 * bar.u
                rightPadding: 2.5 * bar.u
                onPressed: bar.openMenu(index, false)
                onHoveredChanged: if (hovered && dropdown.shown && bar.current !== index) bar.openMenu(index, false)
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
        onPressed: everything.shown || dropdown.shown ? (everything.close(), dropdown.close()) : bar.openEverything(false)
        Accessible.name: qsTr("Menu")
        Accessible.role: Accessible.ButtonMenu
        background: Rectangle {
            color: everything.shown || dropdown.shown ? bar.look.body
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

    CommandMenu {
        id: dropdown
        objectName: "menuDropdown"
        onStepRequested: delta => bar.openMenu(bar.current + delta, true)
        // A press on its own title (its parent) is the title's: it closes it.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    }
    CommandMenu {
        id: everything
        objectName: "menuEverything"
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    }
}
