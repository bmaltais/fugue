pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"

// Node search (spec 12.8, docs/canvas-look.md 12): a query field over a list of results, each
// with its category, ports and version. Every decision (ranking, filtering by the dragged
// link's port, what accepting does) is in NodeSearch (node_search.h); this only shows it.
// Needs the `canvasModel` and `theme` context properties, and the canvas, to open where the
// node will go.
Popup {
    id: root
    objectName: "nodeSearch"

    property Item canvas // the FugueCanvas, to put the popup's corner at the search point
    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    readonly property var search: App.canvasModel.search
    readonly property var look: App.theme.look
    readonly property real u: look.unit
    property alias queryField: field
    property alias resultList: list

    parent: Overlay.overlay
    anchors.centerIn: undefined
    width: Math.min(parent.width - 8 * u, 120 * u)
    height: Math.min(parent.height * 0.7, contentColumn.implicitHeight)
    // Its top left at the point the node goes (the pointer, or a dropped link's end), kept
    // inside the window; mirrored right to left, its top right.
    function place() {
        let px = (parent.width - width) / 2, py = parent.height * 0.12
        if (canvas) {
            const p = canvas.mapToItem(null, canvas.panX + search.point.x * canvas.zoom,
                                       canvas.panY + search.point.y * canvas.zoom)
            px = rightToLeft ? p.x - width : p.x
            py = p.y
        }
        x = Math.round(Math.max(2 * u, Math.min(px, parent.width - width - 2 * u)))
        y = Math.round(Math.max(2 * u, Math.min(py, parent.height - height - 2 * u)))
    }
    modal: true
    dim: false
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; to: 0; duration: 90; easing.type: Easing.OutCubic }
    }
    transformOrigin: rightToLeft ? Popup.TopRight : Popup.TopLeft

    // The search's state is the model's; the popup follows it.
    onAboutToShow: place()
    onHeightChanged: if (opened) place()
    onOpened: {
        field.forceActiveFocus()
        list.currentIndex = 0
    }
    onClosed: root.search.close()
    Connections {
        target: root.search
        function onActiveChanged() {
            if (root.search.active && !root.opened)
                root.open()
            else if (!root.search.active && root.opened)
                root.close()
        }
        function onResultsChanged() { list.currentIndex = root.search.count > 0 ? 0 : -1 }
    }

    background: PopupSurface { look: root.look }

    function accept(row) {
        if (row >= 0)
            root.search.accept(row)
    }

    // The query's words, for underlining what matched in a name.
    readonly property var words: search.query.toLowerCase().split(/\s+/).filter(w => w.length > 0)
    function escaped(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }
    // The name with each word of the query found in it marked: the accent and an underline.
    function marked(name) {
        if (words.length === 0)
            return escaped(name)
        const lower = name.toLowerCase(), hit = new Array(name.length).fill(false)
        for (const w of words) {
            const at = lower.indexOf(w)
            if (at >= 0)
                for (let i = at; i < at + w.length; ++i)
                    hit[i] = true
        }
        let out = "", open = false
        for (let i = 0; i < name.length; ++i) {
            if (hit[i] && !open) {
                out += "<u><font color=\"" + look.accent + "\">"
                open = true
            } else if (!hit[i] && open) {
                out += "</font></u>"
                open = false
            }
            out += escaped(name[i])
        }
        return out + (open ? "</font></u>" : "")
    }
    // "Image/Transform" and its pack: "Image › Transform · fugue".
    function where(category, typeId) {
        const pack = typeId.indexOf("/") > 0 ? typeId.slice(0, typeId.indexOf("/")) : ""
        return category.split("/").join(" › ") + (pack !== "" ? " · " + pack : "")
    }

    contentItem: Item {
        LayoutMirroring.enabled: root.rightToLeft
        LayoutMirroring.childrenInherit: true

        Column {
            id: contentColumn
            width: parent.width

            // The field: a search glyph, the query, an Esc keycap.
            Item {
                width: parent.width
                height: 11 * root.u
                Glyph {
                    id: searchGlyph
                    x: 4 * root.u
                    anchors.verticalCenter: parent.verticalCenter
                    name: "search"
                    look: root.look
                    color: root.look.label
                    font.pixelSize: 16
                }
                // A plain TextInput, so no control style draws over the look.
                TextInput {
                    id: field
                    objectName: "searchField"
                    x: searchGlyph.x + searchGlyph.width + 2.5 * root.u
                    width: escCap.x - x - 2 * root.u
                    anchors.verticalCenter: parent.verticalCenter
                    clip: true
                    color: root.look.text
                    selectionColor: root.look.selectedRow
                    selectedTextColor: root.look.text
                    selectByMouse: true
                    font.pixelSize: root.look.searchFieldSize
                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Search nodes")
                    text: root.search.query
                    onTextEdited: root.search.query = text
                    Keys.onDownPressed: list.currentIndex = Math.min(list.currentIndex + 1, list.count - 1)
                    Keys.onUpPressed: list.currentIndex = Math.max(list.currentIndex - 1, 0)
                    Keys.onReturnPressed: root.accept(list.currentIndex)
                    Keys.onEnterPressed: root.accept(list.currentIndex)
                    Text {
                        visible: field.text === ""
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Search nodes")
                        color: root.look.label
                        font: field.font
                    }
                }
                Keycap {
                    id: escCap
                    x: parent.width - width - 3 * root.u
                    anchors.verticalCenter: parent.verticalCenter
                    look: root.look
                    text: "Esc"
                }
            }

            // From a dropped link: the type filter, as a chip with an × that removes it.
            Item {
                visible: root.search.filterChip !== ""
                width: parent.width
                height: visible ? 8.5 * root.u : 0
                Rectangle {
                    objectName: "searchFilter"
                    readonly property string text: root.search.filterText
                    visible: parent.visible
                    x: 4 * root.u
                    width: chipRow.implicitWidth + 4 * root.u
                    height: 6 * root.u
                    radius: Math.min(root.look.popupCorner, 12)
                    color: "transparent"
                    border.width: 1
                    border.color: root.look.hairlineStrong
                    Accessible.name: text
                    Row {
                        id: chipRow
                        x: 2 * root.u
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 1.5 * root.u
                        PortDot {
                            anchors.verticalCenter: parent.verticalCenter
                            scale: 0.7
                            shape: root.search.link.shape || "circle"
                            fill: root.look.port(root.search.link.color || "unknown")
                            body: root.look.popup
                            corner: root.look.corner
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.search.filterChip
                            color: root.look.text
                            font.pixelSize: root.look.labelSize
                        }
                        Glyph {
                            objectName: "clearFilter"
                            anchors.verticalCenter: parent.verticalCenter
                            name: "x"
                            look: root.look
                            color: root.look.label
                            font.pixelSize: 12
                            TapHandler { onTapped: root.search.clearFilter() }
                        }
                    }
                }
            }
            Rectangle {
                width: parent.width
                height: 1
                color: root.look.hairline
            }

            ListView {
                id: list
                objectName: "searchResults"
                width: parent.width
                height: Math.min(contentHeight + 3 * root.u, root.parent ? root.parent.height * 0.7 - 11 * root.u
                                 - 8.5 * root.u - (root.search.filterChip !== "" ? 8.5 * root.u : 0) - 2 : 400)
                topMargin: 1.5 * root.u
                bottomMargin: 1.5 * root.u
                leftMargin: 1.5 * root.u
                rightMargin: 1.5 * root.u
                clip: true
                model: root.search.results
                currentIndex: 0
                highlightMoveDuration: 0
                highlightFollowsCurrentItem: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                // With no query the results are by category: each gets a heading.
                section.property: root.words.length === 0 ? "category" : ""
                section.delegate: Text {
                    required property string section
                    width: ListView.view.width - 3 * root.u
                    height: 7 * root.u
                    leftPadding: 2 * root.u
                    verticalAlignment: Text.AlignBottom
                    bottomPadding: 1.5 * root.u
                    text: section.split("/").join(" › ")
                    color: root.look.label
                    font.pixelSize: root.look.captionSize
                    font.weight: Font.DemiBold
                }
                highlight: Rectangle {
                    radius: Math.max(0, root.look.popupCorner - 6)
                    color: root.look.selectedRow
                }

                delegate: Item {
                    id: row
                    objectName: "searchResult"
                    required property int index
                    required property string typeId
                    required property string name
                    required property string category
                    required property string version
                    required property string description
                    required property var inputs
                    required property var outputs
                    width: ListView.view.width - 3 * root.u
                    height: 11 * root.u

                    TapHandler { onTapped: root.accept(row.index) }
                    HoverHandler { onHoveredChanged: if (hovered) list.currentIndex = row.index }

                    // A tile with the node's kind: its first output's mark on that color at 16%.
                    readonly property var kind: outputs.length > 0 ? outputs[0] : inputs.length > 0 ? inputs[0] : null
                    Rectangle {
                        id: tile
                        x: 2 * root.u
                        anchors.verticalCenter: parent.verticalCenter
                        width: 7 * root.u
                        height: 7 * root.u
                        radius: Math.min(root.look.corner, 4)
                        color: Qt.tint(root.look.popup, Qt.alpha(root.look.port(row.kind ? row.kind.color : "unknown"), 0.16))
                        Rectangle {
                            anchors.centerIn: parent
                            readonly property string shape: row.kind ? row.kind.shape : "circle"
                            width: 10
                            height: 10
                            radius: shape === "circle" || shape === "ring" ? 5 : Math.min(root.look.corner, 1.5)
                            rotation: shape === "diamond" ? 45 : 0
                            scale: shape === "diamond" ? 0.85 : 1
                            color: shape === "ring" ? "transparent" : root.look.port(row.kind ? row.kind.color : "unknown")
                            border.width: shape === "ring" ? 1.5 : 0
                            border.color: root.look.port(row.kind ? row.kind.color : "unknown")
                        }
                    }
                    Text {
                        objectName: "resultName"
                        x: tile.x + tile.width + 2.5 * root.u
                        y: parent.height / 2 - height + 1
                        width: signature.x - x - 2.5 * root.u
                        text: root.marked(row.name)
                        textFormat: Text.StyledText
                        elide: Text.ElideRight
                        color: root.look.text
                        font.pixelSize: root.look.resultSize
                        font.weight: Font.DemiBold
                    }
                    Text {
                        objectName: "resultCategory"
                        x: tile.x + tile.width + 2.5 * root.u
                        y: parent.height / 2 + 1
                        width: signature.x - x - 2.5 * root.u
                        text: root.where(row.category, row.typeId)
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: root.look.label
                        font.pixelSize: root.look.labelSize
                    }
                    // The port signature: input marks → output marks, 8 px; with a dragged
                    // link, the port it would use in full and the others quieter.
                    Row {
                        id: signature
                        objectName: "resultPorts"
                        x: versionText.x - width - 2.5 * root.u
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Repeater {
                            model: row.inputs.length > 6 ? row.inputs.slice(0, 6) : row.inputs
                            delegate: SignatureMark {}
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            leftPadding: 2
                            rightPadding: 2
                            text: "→"
                            color: root.look.label
                            font.pixelSize: root.look.captionSize
                        }
                        Repeater {
                            model: row.outputs.length > 4 ? row.outputs.slice(0, 4) : row.outputs
                            delegate: SignatureMark {}
                        }
                    }
                    Text {
                        id: versionText
                        objectName: "resultVersion"
                        x: parent.width - width - 2.5 * root.u
                        anchors.verticalCenter: parent.verticalCenter
                        // Trailing zero parts dropped, as the mockup: 1.0.0 is "v1", 1.2.0 "v1.2".
                        text: "v" + row.version.replace(/(\.0)+$/, "")
                        color: root.look.label
                        font.pixelSize: root.look.labelSize
                        font.features: { "tnum": 1 }
                    }
                }

                Column {
                    objectName: "noResults"
                    visible: list.count === 0
                    anchors.centerIn: parent
                    width: parent.width - 8 * root.u
                    spacing: root.u
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: root.search.query !== "" ? qsTr("No node matches “%1”.").arg(root.search.query)
                                                      : qsTr("No node matches")
                        wrapMode: Text.Wrap
                        color: root.look.text
                        font.pixelSize: root.look.textSize
                    }
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("Search looks at names, categories and port types.")
                        wrapMode: Text.Wrap
                        color: root.look.label
                        font.pixelSize: root.look.labelSize
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: root.look.hairline
            }
            // The footer: the keys, and how many results of how many types.
            Item {
                width: parent.width
                height: 8.5 * root.u
                Row {
                    x: 4 * root.u
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: root.u
                    Keycap { look: root.look; text: "arrow-up"; glyph: true }
                    Keycap { look: root.look; text: "arrow-down"; glyph: true }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        rightPadding: 2 * root.u
                        text: qsTr("Move")
                        color: root.look.label
                        font.pixelSize: root.look.labelSize
                    }
                    Keycap { look: root.look; text: "corner-down-left"; glyph: true }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Add")
                        color: root.look.label
                        font.pixelSize: root.look.labelSize
                    }
                }
                Text {
                    x: parent.width - width - 4 * root.u
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("%1 of %2").arg(root.search.count).arg(root.search.total)
                    color: root.look.label
                    font.pixelSize: root.look.labelSize
                    font.features: { "tnum": 1 }
                }
            }
        }
    }

    component SignatureMark: Rectangle {
        required property var modelData
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        width: 8
        height: 8
        radius: modelData.shape === "circle" || modelData.shape === "ring" ? 4 : Math.min(root.look.corner, 1.5)
        rotation: modelData.shape === "diamond" ? 45 : 0
        scale: modelData.shape === "diamond" ? 0.8 : 1
        color: modelData.shape === "ring" ? "transparent" : root.look.port(modelData.color)
        border.width: modelData.shape === "ring" ? 1.5 : 0
        border.color: root.look.port(modelData.color)
        opacity: root.search.link.node === undefined || modelData.match ? 1 : 0.55
    }
}
