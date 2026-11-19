import QtQuick
import QtQuick.Controls

// The list a choice, seed or model widget opens: entries of {text, value, detail, checked,
// enabled}. Modal, so the window's keys sleep while it is open; Up, Down and Enter choose,
// Escape or a press outside closes it.
Popup {
    id: menu
    objectName: "widgetMenu"
    required property var look
    property var entries: []
    signal picked(var value)

    readonly property real rowHeight: Math.round(look.textSize * 2)
    modal: true
    dim: false
    focus: true
    padding: 2
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    // No transitions: the next click on the canvas must not land on a menu still closing.
    enter: null
    exit: null
    height: Math.min(list.contentHeight, rowHeight * 12) + 2 * padding

    background: Rectangle {
        color: menu.look.panel
        border.width: 1
        border.color: menu.look.border
        radius: Math.min(menu.look.corner, 8)
    }

    contentItem: ListView {
        id: list
        objectName: "widgetMenuList"
        model: menu.entries
        focus: true
        clip: true
        keyNavigationEnabled: true
        boundsBehavior: Flickable.StopAtBounds
        currentIndex: {
            for (let i = 0; i < menu.entries.length; ++i)
                if (menu.entries[i].checked)
                    return i
            return 0
        }
        delegate: Rectangle {
            id: entry
            required property var modelData
            required property int index
            readonly property bool usable: modelData.enabled !== false
            width: ListView.view.width
            height: menu.rowHeight
            color: ListView.isCurrentItem && usable ? Qt.tint(menu.look.panel, Qt.alpha(menu.look.accent, 0.16))
                                                    : menu.look.panel
            Text {
                x: 8
                width: parent.width - 16 - detail.width
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: (entry.modelData.checked ? "✓ " : "") + entry.modelData.text
                color: entry.usable ? menu.look.text : menu.look.muted
                font.pixelSize: menu.look.smallText
            }
            Text {
                id: detail
                anchors.right: parent.right
                anchors.rightMargin: 8
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                textFormat: Text.PlainText
                text: entry.modelData.detail !== undefined ? entry.modelData.detail : ""
                color: menu.look.muted
                font.pixelSize: menu.look.smallText
            }
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                onEntered: if (entry.usable) list.currentIndex = entry.index
                onClicked: menu.choose(entry.index)
            }
        }
        Keys.onReturnPressed: menu.choose(currentIndex)
        Keys.onEnterPressed: menu.choose(currentIndex)
    }

    function choose(index) {
        const chosen = entries[index]
        if (!chosen || chosen.enabled === false)
            return
        close()
        picked(chosen.value)
    }
}
