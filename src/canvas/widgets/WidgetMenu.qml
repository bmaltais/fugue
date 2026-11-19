import QtQuick
import QtQuick.Controls
import ".."

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
    // An entry with a `thumbnail` (a file URL) shows it small at the left, in a taller row.
    readonly property real thumbHeight: Math.round(look.textSize * 3.2)
    modal: true
    dim: false
    focus: true
    padding: 2
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    // No transitions: the next click on the canvas must not land on a menu still closing.
    enter: null
    exit: null
    height: Math.min(list.contentHeight, rowHeight * 12) + 2 * padding

    // A level 3 surface (docs/canvas-look.md 7), like node search.
    background: PopupSurface { look: menu.look }

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
            readonly property bool hasThumbnail: modelData.thumbnail !== undefined
            height: hasThumbnail ? menu.thumbHeight : menu.rowHeight
            radius: Math.max(0, menu.look.popupCorner - 2)
            color: ListView.isCurrentItem && usable ? menu.look.selectedRow : "transparent"
            Glyph {
                id: check
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: entry.modelData.checked === true
                name: "check"
                look: menu.look
                color: menu.look.accent
                font.pixelSize: 12
            }
            // Decoded small and only as the row scrolls into view.
            Image {
                visible: entry.hasThumbnail
                x: 8
                y: 3
                width: parent.height - 6
                height: width
                source: entry.hasThumbnail ? entry.modelData.thumbnail : ""
                sourceSize: Qt.size(96, 96)
                asynchronous: true
                cache: false
                fillMode: Image.PreserveAspectCrop
            }
            Text {
                x: entry.hasThumbnail ? parent.height + 4 : 8 + 12 + 6
                width: parent.width - x - 8 - detail.width
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: entry.modelData.text
                color: entry.usable ? menu.look.text : menu.look.label
                font.pixelSize: menu.look.labelSize
            }
            Text {
                id: detail
                anchors.right: parent.right
                anchors.rightMargin: 8
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                textFormat: Text.PlainText
                text: entry.modelData.detail !== undefined ? entry.modelData.detail : ""
                color: menu.look.label
                font.pixelSize: menu.look.labelSize
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
