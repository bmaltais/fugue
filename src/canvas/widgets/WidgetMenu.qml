pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import ".."
import "../../menu/placement.js" as Placement

// The list a choice, seed or model widget opens: entries of {text, value, detail, note,
// checked, enabled, heading, separator, keep, dim, keywords, place, placeHost}. A `heading` names the group of entries below it, a
// `separator` (with `enabled: false`) is a hairline between two chunks that need no name (the Gaps rule); a
// `note` is a second, smaller line; a `dim` entry can be chosen but reads as one the list
// advises against. Modal, so the window's keys sleep while it is open; Up and Down move
// between the entries that can be chosen, Enter chooses, Escape or a press outside closes it.
//
// `searchable` puts a search field on top, holding the keys (#84): what is typed filters the
// entries to those with every word in their text, detail, keywords or heading, case aside; a heading
// stays while an entry under it does, an entry with `keep` (an action such as "Type a name…")
// always does. Escape empties the field before it closes the menu.
Popup {
    id: menu
    objectName: "widgetMenu"
    required property CanvasLook look
    property var entries: []
    property bool searchable: false
    // Each typed word must begin a word of the entry (as the templates window's search, engine/template_search.cpp), not
    // sit anywhere in it. Off: a word may be anywhere (the model menus').
    property bool wordStart: false
    property string query: ""
    // How many rows tall the list grows before it scrolls (a menu of a few grouped services sets more).
    property int maxRows: 12
    // What `nothing` says when no entry matches the query.
    property string nothingMatches: qsTr("Nothing matches “%1”")
    // The entries the query leaves (all of them with none), as the list shows them.
    readonly property var shown: filtered(entries, query)
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
    // Where it wants to open (under the widget, in the overlay's coordinates), and the widget's
    // height: below it, or above it when there is no room. Not `x` and `y`: a closed Popup reads
    // them back as 0 until it is shown, so aboutToShow would place it from the corner.
    property real wantX: 0
    property real wantY: 0
    property real anchorHeight: 0
    readonly property Item overlay: Overlay.overlay
    margins: Placement.margin
    height: Math.min(Math.min(list.contentHeight, rowHeight * maxRows) + searchHeight + 2 * padding,
                     overlay ? Placement.maxExtent(overlay.height) : Infinity)
    readonly property real searchHeight: searchable ? rowHeight + 2 : 0
    // Flipped above the widget, or shifted, so it stays inside the window (placement.js).
    onAboutToShow: {
        if (!overlay)
            return
        const p = Placement.fit(overlay, overlay, wantX, wantY, width, height, 0, anchorHeight)
        x = p.x
        y = p.y
    }

    // A level 3 surface (docs/canvas-look.md 7), like node search.
    background: PopupSurface { look: menu.look }

    contentItem: Item {
    // The search field: a plain TextInput, so no control style draws over the look (as node
    // search's). It holds the keys and passes the list's on.
    Item {
        visible: menu.searchable
        width: parent.width
        height: menu.searchHeight
        Glyph {
            id: searchGlyph
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            look: menu.look
            color: menu.look.label
            font.pixelSize: 12
        }
        TextInput {
            id: field
            objectName: "widgetMenuSearch"
            x: searchGlyph.x + searchGlyph.width + 6
            width: parent.width - x - 8
            anchors.verticalCenter: parent.verticalCenter
            clip: true
            focus: menu.searchable
            color: menu.look.text
            selectionColor: menu.look.accent
            selectedTextColor: menu.look.accentForeground
            selectByMouse: true
            font.pixelSize: menu.look.labelSize
            Accessible.role: Accessible.EditableText
            Accessible.name: qsTr("Search")
            text: menu.query
            onTextEdited: menu.query = text
            Text {
                visible: field.text === ""
                text: qsTr("Search")
                color: menu.look.label
                font: field.font
            }
            Keys.onDownPressed: menu.move(1)
            Keys.onUpPressed: menu.move(-1)
            Keys.onReturnPressed: menu.choose(list.currentIndex)
            Keys.onEnterPressed: menu.choose(list.currentIndex)
            Keys.onEscapePressed: event => {
                if (menu.query === "") {
                    menu.close()
                } else {
                    menu.query = ""
                }
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: menu.look.menuRow
        }
    }
    ListView {
        id: list
        objectName: "widgetMenuList"
        y: menu.searchHeight
        width: parent.width
        height: parent.height - y
        model: menu.shown
        focus: !menu.searchable
        clip: true
        // The width the scroll bar covers, kept clear by the rows' right-hand text.
        readonly property real edge: (ScrollBar.vertical as FugueScrollBar).inset
        keyNavigationEnabled: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: FugueScrollBar { look: menu.look }
        // Home and End reach the ends of a long list when the list has the keys.
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Home) { currentIndex = Math.max(0, menu.nextUsable(-1, 1)); event.accepted = true }
            else if (event.key === Qt.Key_End) { currentIndex = Math.max(0, menu.nextUsable(menu.shown.length, -1)); event.accepted = true }
        }
        // Up and Down keep the current entry in view when the list is capped and scrolls.
        onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
        // The checked entry, else the first that can be chosen; again whenever the query changes.
        Component.onCompleted: currentIndex = menu.firstIndex()
        Connections {
            target: menu
            function onShownChanged() { list.currentIndex = menu.firstIndex() }
        }
        delegate: Rectangle {
            id: entry
            required property var modelData
            required property int index
            readonly property bool usable: modelData.enabled !== false
            width: ListView.view.width - list.edge // the highlight stays out of the scroll bar's gutter
            readonly property bool hasThumbnail: modelData.thumbnail !== undefined
            readonly property bool heading: modelData.heading === true
            readonly property bool separator: modelData.separator === true
            readonly property string note: modelData.note !== undefined ? modelData.note : ""
            height: separator ? 2 * menu.look.unit
                  : hasThumbnail ? menu.thumbHeight
                  : note !== "" ? menu.rowHeight + noteText.implicitHeight
                  : menu.rowHeight
            radius: Math.min(menu.look.params.cornerChip * menu.look.unit, 0.45 * height) // the chip corner, as every menu row (look parameters)
            color: ListView.isCurrentItem && usable ? menu.look.menuRow : "transparent"
            Rectangle {
                visible: entry.separator
                x: 8
                width: parent.width - 16 - list.edge
                anchors.verticalCenter: parent.verticalCenter
                height: 1
                color: menu.look.hairline
            }
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
                visible: !entry.separator
                x: entry.hasThumbnail ? parent.height + 4 : entry.heading ? 8 : 8 + 12 + 6
                width: parent.width - x - 8 - detail.width - list.edge - (placeBadge.visible ? placeBadge.width + 4 : 0)
                height: entry.hasThumbnail ? parent.height : menu.rowHeight
                verticalAlignment: entry.heading ? Text.AlignBottom : Text.AlignVCenter
                bottomPadding: entry.heading ? 2 : 0
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: entry.modelData.text ?? "" // a separator has none
                color: entry.usable && entry.modelData.dim !== true ? menu.look.text : menu.look.label
                // Fainter still: the label color alone reads close to the text's in a dark theme.
                opacity: entry.modelData.dim === true ? 0.55 : 1
                font.pixelSize: entry.heading ? menu.look.captionSize : menu.look.labelSize
                font.weight: entry.heading ? Font.DemiBold : Font.Normal
            }
            Text {
                id: noteText
                objectName: "widgetMenuNote"
                visible: entry.note !== ""
                x: 8 + 12 + 6
                y: menu.rowHeight - 4
                width: parent.width - x - 8 - list.edge
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: entry.note
                color: menu.look.label
                font.pixelSize: menu.look.captionSize
                bottomPadding: 6
            }
            // Where the model lives (#164): this machine, a paired host or a cloud service, left of the detail.
            PlaceBadge {
                id: placeBadge
                objectName: "widgetMenuPlace"
                visible: entry.modelData.place !== undefined && !entry.heading
                anchors.right: detail.left
                anchors.rightMargin: 4
                y: (menu.rowHeight - height) / 2
                look: menu.look
                kind: entry.modelData.place === "host" ? "host" : entry.modelData.place === "cloud" ? "cloud" : "local"
                services: entry.modelData.placeHost !== undefined ? [entry.modelData.placeHost] : []
                hosts: entry.modelData.placeHost !== undefined ? [entry.modelData.placeHost] : []
                offline: entry.modelData.dim === true && entry.modelData.place === "host"
            }
            Text {
                id: detail
                anchors.right: parent.right
                anchors.rightMargin: 8 + list.edge
                height: menu.rowHeight
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
        Keys.onDownPressed: menu.move(1)
        Keys.onUpPressed: menu.move(-1)
        Keys.onReturnPressed: menu.choose(currentIndex)
        Keys.onEnterPressed: menu.choose(currentIndex)
    }
    }

    function filtered(all, text) {
        const words = text.toLowerCase().split(/\s+/).filter(w => w !== "")
        if (words.length === 0)
            return all
        const out = []
        let heading = null      // the heading of the entries being looked at
        let headingShown = false
        let matched = false
        for (const e of all) {
            if (e.heading === true) {
                heading = e
                headingShown = false
                continue
            }
            if (e.separator === true)
                continue // a search result is one list
            const said = [e.text, e.detail, heading ? heading.text : "", e.keywords].join(" ").toLowerCase()
            const fits = e.keep !== true && e.value !== undefined && words.every(w => wordStart ? startsAWord(said, w) : said.includes(w))
            if (!fits && e.keep !== true)
                continue
            if (fits && heading && !headingShown) {
                out.push(heading)
                headingShown = true
            }
            if (e.keep === true && !matched)
                out.push({ text: nothingMatches.arg(text.trim()), enabled: false })
            matched = matched || fits || e.keep === true
            out.push(e)
        }
        if (!matched)
            out.push({ text: nothingMatches.arg(text.trim()), enabled: false })
        return out
    }

    // Whether `word` (lower case) begins a word of `text` (lower case): at its start or after a non-letter, non-digit.
    function startsAWord(text, word) {
        for (let at = text.indexOf(word); at >= 0; at = text.indexOf(word, at + 1))
            if (at === 0 || !isWordChar(text[at - 1]))
                return true
        return false
    }

    // A letter (of any script: its case differs, or it is a CJK character that has none) or a digit.
    function isWordChar(c) {
        return c.toLowerCase() !== c.toUpperCase() || (c >= "0" && c <= "9") || c.charCodeAt(0) > 0x2e7f
    }

    function firstIndex() {
        if (query === "")
            for (let i = 0; i < shown.length; ++i)
                if (shown[i].checked)
                    return i
        return Math.max(0, nextUsable(-1, 1))
    }
    function move(step) {
        const next = nextUsable(list.currentIndex, step)
        if (next >= 0)
            list.currentIndex = next
    }

    // The next entry after `from` in the direction `step` that can be chosen; -1 when none.
    function nextUsable(from, step) {
        for (let i = from + step; i >= 0 && i < shown.length; i += step)
            if (shown[i].enabled !== false)
                return i
        return -1
    }

    function choose(index) {
        const chosen = shown[index]
        if (!chosen || chosen.enabled === false)
            return
        close()
        picked(chosen.value)
    }
}
