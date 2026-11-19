import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Node search (spec 12.8): a query box over a list of results, each with its category,
// ports and version. Every decision (ranking, filtering by the dragged link's port, what
// accepting does) is in NodeSearch (node_search.h); this only shows it. Needs the
// `canvasModel` and `theme` context properties.
Popup {
    id: root
    objectName: "nodeSearch"

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    readonly property var search: canvasModel.search
    property alias queryField: field
    property alias resultList: list

    parent: Overlay.overlay
    anchors.centerIn: undefined
    x: Math.round((parent.width - width) / 2)
    y: Math.round(parent.height * 0.12)
    width: Math.min(parent.width - 48, 560)
    height: Math.min(parent.height - y - 24, 480)
    modal: true
    focus: true
    padding: 12
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // The search's state is the model's; the popup follows it.
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

    background: Rectangle {
        color: theme.panel
        border.width: 1
        border.color: theme.border
        radius: Math.min(theme.radius, 8)
    }

    function accept(row) {
        if (row >= 0)
            root.search.accept(row)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        LayoutMirroring.enabled: root.rightToLeft
        LayoutMirroring.childrenInherit: true

        TextField {
            id: field
            objectName: "searchField"
            Layout.fillWidth: true
            placeholderText: qsTr("Search nodes")
            Accessible.name: qsTr("Search nodes")
            text: root.search.query
            onTextEdited: root.search.query = text
            Keys.onDownPressed: list.currentIndex = Math.min(list.currentIndex + 1, list.count - 1)
            Keys.onUpPressed: list.currentIndex = Math.max(list.currentIndex - 1, 0)
            Keys.onReturnPressed: root.accept(list.currentIndex)
            Keys.onEnterPressed: root.accept(list.currentIndex)
        }

        Label {
            objectName: "searchFilter"
            Layout.fillWidth: true
            visible: root.search.filterText !== ""
            text: root.search.filterText
            wrapMode: Text.Wrap
            color: theme.accent
        }

        ListView {
            id: list
            objectName: "searchResults"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: root.search.results
            currentIndex: 0
            highlightMoveDuration: 0
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
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
                width: ListView.view.width
                highlighted: ListView.isCurrentItem
                onClicked: root.accept(index)
                onHoveredChanged: if (hovered) list.currentIndex = index

                contentItem: ColumnLayout {
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            objectName: "resultName"
                            text: row.name
                            font.bold: true
                        }
                        Label {
                            objectName: "resultVersion"
                            text: "v" + row.version
                            opacity: 0.6
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            objectName: "resultCategory"
                            text: row.category
                            opacity: 0.7
                            elide: Text.ElideRight
                        }
                    }
                    Flow {
                        objectName: "resultPorts"
                        Layout.fillWidth: true
                        spacing: 8
                        Repeater {
                            model: row.inputs
                            delegate: Label {
                                required property var modelData
                                text: "→ " + modelData.label + " (" + modelData.type + ")"
                                font.bold: modelData.match
                                color: modelData.match ? theme.accent : theme.text
                                opacity: modelData.match ? 1 : 0.7
                                font.pixelSize: Math.round(theme.textSize * 0.9)
                            }
                        }
                        Repeater {
                            model: row.outputs
                            delegate: Label {
                                required property var modelData
                                text: modelData.label + " (" + modelData.type + ") →"
                                font.bold: modelData.match
                                color: modelData.match ? theme.accent : theme.text
                                opacity: modelData.match ? 1 : 0.7
                                font.pixelSize: Math.round(theme.textSize * 0.9)
                            }
                        }
                    }
                }
            }

            Label {
                objectName: "noResults"
                anchors.centerIn: parent
                visible: list.count === 0
                text: qsTr("No node matches")
                opacity: 0.7
            }
        }
    }
}
