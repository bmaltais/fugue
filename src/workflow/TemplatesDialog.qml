pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// The Templates dialog (spec 11.10, #83), opened by file.templates (File > Browse templates):
// the categories on the left, a search field and the templates as cards on the right. A card
// shows its preview, title, description and, when this machine lacks something it needs, what
// (models, packs), with a Download button for its missing models that have a source (#89), and
// their progress while they come. Clicking a card, or Enter on it, opens it as a new workflow and
// closes the dialog; the banner then names each missing model, where to get it and where it goes.
// Styled as the settings panel: scrim, popup card, level 3 shadow.
Popup {
    id: dialog
    objectName: "templatesDialog"

    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    // Read again each time it opens: templates and model folders change outside Fugue.
    property var templates: []
    // "" is every template, "own" the person's own, else a category id.
    property string category: ""

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(960, (parent ? parent.width : 960) - 12 * u)
    height: Math.min(640, (parent ? parent.height : 640) - 12 * u)
    Overlay.modal: Rectangle { color: dialog.look.scrim }
    background: PopupSurface { look: dialog.look }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 140; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; to: 0; duration: 90; easing.type: Easing.OutCubic }
    }
    onAboutToShow: {
        templates = App.workflows.templates()
        search.text = ""
        if (category !== "" && !categories.some(c => c.id === category))
            category = ""
    }
    onOpened: search.forceActiveFocus()
    // Models that arrived change what the cards say.
    Connections {
        target: App.workflows.downloads
        enabled: dialog.visible
        function onFinished() { dialog.templates = App.workflows.templates() }
    }

    // The tabs on the left: All, the person's own when there are any, then each category in
    // the order the templates come in (categories.json's).
    readonly property var categories: {
        const list = [{ id: "", title: qsTr("All") }]
        if (templates.some(t => t.own))
            list.push({ id: "own", title: qsTr("Your templates") })
        const seen = {}
        for (const t of templates) {
            if (t.own || t.category === "" || seen[t.category])
                continue
            seen[t.category] = true
            list.push({ id: t.category, title: t.categoryTitle })
        }
        return list
    }

    // The templates of the tab, narrowed by every word typed (title, description, tags,
    // category); in list() order.
    readonly property var shown: {
        const words = search.text.toLowerCase().split(/\s+/).filter(w => w.length > 0)
        return templates.filter(t => {
            if (category === "own" ? !t.own : category !== "" && (t.own || t.category !== category))
                return false
            const text = [t.title, t.description, t.categoryTitle].concat(t.tags).join(" ").toLowerCase()
            return words.every(w => text.indexOf(w) >= 0)
        })
    }

    function openTemplate(id) {
        if (App.workflows.openTemplateById(id))
            close()
    }

    contentItem: RowLayout {
        spacing: 0
        LayoutMirroring.enabled: dialog.rightToLeft
        LayoutMirroring.childrenInherit: true

        // The categories, and where the person's own templates go.
        ColumnLayout {
            Layout.preferredWidth: 44 * dialog.u
            Layout.fillWidth: false
            Layout.fillHeight: true
            Layout.margins: 4 * dialog.u
            spacing: 0
            Text {
                Layout.bottomMargin: 4 * dialog.u
                Layout.leftMargin: 2 * dialog.u
                text: qsTr("Templates")
                color: dialog.look.text
                font.pixelSize: dialog.look.overlayTitleSize
                font.weight: Font.DemiBold
            }
            Repeater {
                model: dialog.categories
                delegate: Rectangle {
                    id: tab
                    required property var modelData
                    required property int index
                    readonly property bool current: dialog.category === modelData.id
                    objectName: "templatesCategory-" + (modelData.id === "" ? "all" : modelData.id)
                    Layout.fillWidth: true
                    implicitHeight: 7 * dialog.u
                    radius: dialog.look.smallCorner
                    color: current ? dialog.look.selectedRow : tabHover.hovered ? dialog.look.well : "transparent"
                    Accessible.role: Accessible.PageTab
                    Accessible.name: modelData.title
                    Accessible.selected: current
                    activeFocusOnTab: true
                    border.width: activeFocus ? 1 : 0
                    border.color: dialog.look.accent
                    Keys.onSpacePressed: dialog.category = modelData.id
                    Keys.onReturnPressed: dialog.category = modelData.id
                    Keys.onUpPressed: if (index > 0) dialog.category = dialog.categories[index - 1].id
                    Keys.onDownPressed: if (index < dialog.categories.length - 1) dialog.category = dialog.categories[index + 1].id
                    HoverHandler { id: tabHover }
                    TapHandler { onTapped: dialog.category = tab.modelData.id }
                    Text {
                        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 2 * dialog.u
                                  right: parent.right; rightMargin: 2 * dialog.u }
                        text: tab.modelData.title
                        elide: Text.ElideRight
                        color: dialog.look.text
                        font.pixelSize: dialog.look.textSize
                        font.weight: tab.current ? Font.DemiBold : Font.Normal
                    }
                }
            }
            Item { Layout.fillHeight: true }
            Text {
                objectName: "templatesOwnFolder"
                Layout.fillWidth: true
                Layout.leftMargin: 2 * dialog.u
                text: qsTr("Your own templates go in %1").arg(App.workflows.ownTemplatesFolder())
                wrapMode: Text.Wrap
                color: dialog.look.label
                font.pixelSize: dialog.look.captionSize
            }
        }
        Rectangle { Layout.fillHeight: true; implicitWidth: 1; color: dialog.look.hairline }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 4 * dialog.u
            spacing: 4 * dialog.u

            ThemedTextField {
                id: search
                objectName: "templatesSearch"
                Layout.fillWidth: true
                placeholderText: qsTr("Search templates")
                font.pixelSize: dialog.look.textSize
                // Enter opens the first match; Down goes to the cards.
                Keys.onDownPressed: { grid.currentIndex = 0; grid.forceActiveFocus() }
                Keys.onReturnPressed: if (dialog.shown.length > 0) dialog.openTemplate(dialog.shown[0].id)
                Keys.onEnterPressed: if (dialog.shown.length > 0) dialog.openTemplate(dialog.shown[0].id)
            }

            GridView {
                id: grid
                objectName: "templatesGrid"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                model: dialog.shown
                keyNavigationEnabled: true
                // As many 56u cards as fit, sharing the spare width.
                readonly property int columns: Math.max(1, Math.floor(width / (56 * dialog.u)))
                cellWidth: Math.floor(width / columns)
                cellHeight: cellWidth * 10 / 16 + 25 * dialog.u
                Keys.onReturnPressed: if (currentIndex >= 0) dialog.openTemplate(dialog.shown[currentIndex].id)
                Keys.onEnterPressed: if (currentIndex >= 0) dialog.openTemplate(dialog.shown[currentIndex].id)

                delegate: Item {
                    id: cell
                    required property var modelData
                    required property int index
                    width: grid.cellWidth
                    height: grid.cellHeight

                    Rectangle {
                        id: card
                        objectName: "templateCard-" + cell.modelData.id
                        anchors.fill: parent
                        anchors.margins: dialog.u
                        radius: dialog.look.corner
                        color: cardHover.hovered || (grid.activeFocus && grid.currentIndex === cell.index)
                               ? dialog.look.well : "transparent"
                        border.width: grid.activeFocus && grid.currentIndex === cell.index ? 1 : 0
                        border.color: dialog.look.accent
                        Accessible.role: Accessible.Button
                        Accessible.name: cell.modelData.title
                        Accessible.description: cell.modelData.description
                        HoverHandler { id: cardHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: dialog.openTemplate(cell.modelData.id) }

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 2 * dialog.u
                            spacing: dialog.u

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: width * 10 / 16
                                radius: dialog.look.thumbCorner
                                color: dialog.look.well
                                clip: true
                                Image {
                                    anchors.fill: parent
                                    source: cell.modelData.preview
                                    visible: source !== ""
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                    sourceSize.width: 512
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: cell.modelData.title
                                elide: Text.ElideRight
                                color: dialog.look.text
                                font.pixelSize: dialog.look.textSize
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                text: cell.modelData.description
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                color: dialog.look.label
                                font.pixelSize: dialog.look.captionSize
                            }
                            RowLayout {
                                id: statusRow
                                readonly property var downloads: App.workflows.downloads
                                readonly property bool downloading: downloads.busy
                                                                    && downloads.what === "template:" + cell.modelData.id
                                Layout.fillWidth: true
                                Layout.preferredHeight: 7 * dialog.u
                                visible: cell.modelData.status !== "" || downloading
                                spacing: dialog.u
                                Text {
                                    objectName: "templateStatus-" + cell.modelData.id
                                    Layout.fillWidth: true
                                    text: statusRow.downloading
                                          ? [statusRow.downloads.label, statusRow.downloads.progressText].filter(t => t !== "").join(" · ")
                                          : cell.modelData.status
                                    elide: Text.ElideRight
                                    color: statusRow.downloading ? dialog.look.label : dialog.look.error
                                    font.pixelSize: dialog.look.captionSize
                                    // Which files, on hover; opening it names where each goes.
                                    ToolTip.visible: statusHover.hovered && cell.modelData.missingModels.length > 0
                                    ToolTip.text: cell.modelData.missingModels.map(m => m.name).join("\n")
                                    HoverHandler { id: statusHover }
                                }
                                // Its own press target: a click here does not open the template.
                                ThemedButton {
                                    objectName: "templateDownload-" + cell.modelData.id
                                    visible: statusRow.downloading || (cell.modelData.download !== "" && !statusRow.downloads.busy)
                                    flat: true
                                    implicitHeight: 7 * dialog.u
                                    topInset: 0
                                    bottomInset: 0
                                    font.pixelSize: dialog.look.captionSize
                                    text: statusRow.downloading ? qsTr("Cancel") : qsTr("Download")
                                    Accessible.name: statusRow.downloading ? qsTr("Cancel the download") : cell.modelData.download
                                    ToolTip.visible: hovered && !statusRow.downloading
                                    ToolTip.text: cell.modelData.download
                                    onClicked: statusRow.downloading ? statusRow.downloads.cancel()
                                                                     : App.workflows.downloadTemplate(cell.modelData.id)
                                }
                            }
                        }
                    }
                }

                Text {
                    objectName: "templatesEmpty"
                    anchors.centerIn: parent
                    width: parent.width - 16 * dialog.u
                    visible: dialog.shown.length === 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: dialog.templates.length === 0
                          ? qsTr("No templates here yet. Put your own in %1.").arg(App.workflows.ownTemplatesFolder())
                          : qsTr("No template matches.")
                    color: dialog.look.label
                    font.pixelSize: dialog.look.textSize
                }
            }
        }
    }
}
