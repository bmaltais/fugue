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
// closes the dialog; the small "i" on its picture, or I or Space on the focused card, shows its details first; the banner then names each missing model, where to get it and where it goes.
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
    // Only the templates whose models are all here or on a paired host (#164).
    property bool readyOnly: false
    // "" is every source, else a source's name (template.json's `source.name`, #369); the same for a model family.
    property string source: ""
    property string family: ""
    // The template whose details are shown over the grid ("" none), and the card to give the focus back to.
    property string detailId: ""
    readonly property var detail: detailId === "" ? null : templates.find(t => t.id === detailId) ?? null
    readonly property var noDetail: ({ title: "", description: "", preview: "", categoryTitle: "", source: {}, families: [], tags: [],
                                       modelList: [], packs: [], inputs: [] })
    function showDetails(id, index) {
        detailId = id
        if (index !== undefined)
            grid.currentIndex = index
    }
    // Escape closes the details first, the dialog on the next press (a shortcut, so it holds whatever has the focus).
    Shortcut {
        sequence: "Escape"
        enabled: dialog.visible && dialog.detail !== null
        onActivated: dialog.closeDetails()
    }
    function closeDetails() {
        detailId = ""
        grid.forceActiveFocus()
    }

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(1200, (parent ? parent.width : 960) - 12 * u)
    height: Math.min(800, (parent ? parent.height : 640) - 12 * u)
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
        // The paired hosts' models arrive later (templatesChanged), or at once when already known.
        App.workflows.loadHostModels()
        templates = App.workflows.templates()
        search.text = ""
        source = ""
        family = ""
        detailId = ""
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
    // The paired hosts answered: which templates they can run.
    Connections {
        target: App.workflows
        enabled: dialog.visible
        function onTemplatesChanged() { dialog.templates = App.workflows.templates() }
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

    // Whether a template passes the tab, Ready to run, the search and the two drop-downs, except
    // the one named by `skip` ("source" or "family"): the counts of a drop-down say what choosing
    // each of its entries would leave.
    function passes(t, skip) {
        if (category === "own" ? !t.own : category !== "" && (t.own || t.category !== category))
            return false
        if (readyOnly && !t.ready)
            return false
        if (skip !== "source" && source !== "" && t.source.name !== source)
            return false
        if (skip !== "family" && family !== "" && t.families.indexOf(family) < 0)
            return false
        return App.workflows.templateMatches(search.text, t)
    }

    // One drop-down's entries: "All" first, then each value the templates have, with its count; a value that would leave
    // nothing (with the other choices as they are) is dimmed, as the model menus dim what does not fit.
    function options(all, skip, valuesOf) {
        const counts = {}
        const order = []
        let total = 0
        for (const t of templates) {
            const here = passes(t, skip)
            if (here)
                ++total
            for (const v of valuesOf(t)) {
                if (!(v in counts)) {
                    counts[v] = 0
                    order.push(v)
                }
                if (here)
                    ++counts[v]
            }
        }
        order.sort((a, b) => a.localeCompare(b))
        return [{ value: "", text: qsTr("%1 (%2)").arg(all).arg(total) }]
            .concat(order.map(v => ({ value: v, text: qsTr("%1 (%2)").arg(v).arg(counts[v]), dim: counts[v] === 0 })))
    }
    readonly property var sourceOptions: options(qsTr("All sources"), "source", t => t.source.name ? [t.source.name] : [])
    readonly property var familyOptions: options(qsTr("All model families"), "family", t => t.families)

    // The templates of the tab, narrowed by the search (every word: title, description, tags,
    // category, id, source, family; tag:, source:, family:), the two drop-downs and Ready to run;
    // in list() order, except that within a category the ones ready to run come first (#164).
    readonly property var shown: {
        const found = templates.filter(t => passes(t, ""))
        // A stable partition inside each run of one category: the list comes grouped by it.
        const out = []
        let from = 0
        while (from < found.length) {
            let to = from
            while (to < found.length && found[to].category === found[from].category && found[to].own === found[from].own)
                ++to
            const run = found.slice(from, to)
            out.push(...run.filter(t => t.ready), ...run.filter(t => !t.ready))
            from = to
        }
        return out
    }

    // A card's Download: a template that needs packs opens, and its Get ready sheet does the downloads.
    function downloadTemplate(id) {
        if (App.workflows.downloadTemplate(id))
            close()
    }

    function openTemplate(id) {
        if (App.workflows.openTemplateById(id))
            close()
    }

    contentItem: Item {
        RowLayout {
            id: content
            anchors.fill: parent
            enabled: dialog.detail === null
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

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2 * dialog.u
                // Fugue's well, as the settings' fields and the drop-downs: 7u high, the accent edge while focused.
                Rectangle {
                    Layout.fillWidth: true
                    implicitWidth: 40 * dialog.u
                    implicitHeight: 7 * dialog.u
                    radius: dialog.look.smallCorner
                    color: dialog.look.well
                    border.width: 1
                    border.color: search.activeFocus ? dialog.look.accent : dialog.look.hairlineStrong
                    TextInput {
                        id: search
                        objectName: "templatesSearch"
                        anchors { left: parent.left; right: parent.right; leftMargin: 2 * dialog.u; rightMargin: 2 * dialog.u
                                  verticalCenter: parent.verticalCenter }
                        clip: true
                        selectByMouse: true
                        color: dialog.look.text
                        selectionColor: dialog.look.accent
                        selectedTextColor: dialog.look.accentForeground
                        font.pixelSize: dialog.look.textSize
                        Accessible.role: Accessible.EditableText
                        Accessible.name: qsTr("Search templates")
                        Text {
                            visible: search.text === ""
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Search templates")
                            color: dialog.look.label
                            font: search.font
                        }
                        // Enter opens the first match; Down goes to the cards.
                        Keys.onDownPressed: { grid.currentIndex = 0; grid.forceActiveFocus() }
                        Keys.onReturnPressed: if (dialog.shown.length > 0) dialog.openTemplate(dialog.shown[0].id)
                        Keys.onEnterPressed: if (dialog.shown.length > 0) dialog.openTemplate(dialog.shown[0].id)
                    }
                }
                // The two filters (#369), one drop-down each with the count a choice would leave; a drop-down
                // is there only when some template has a source, or a family, to choose.
                ThemedDropDown {
                    id: sourceBox
                    objectName: "templatesSource"
                    visible: dialog.sourceOptions.length > 1
                    options: dialog.sourceOptions
                    value: dialog.source
                    label: qsTr("Source")
                    searchable: true
                    onPicked: v => dialog.source = v
                }
                ThemedDropDown {
                    id: familyBox
                    objectName: "templatesFamily"
                    visible: dialog.familyOptions.length > 1
                    options: dialog.familyOptions
                    value: dialog.family
                    label: qsTr("Model family")
                    searchable: true
                    onPicked: v => dialog.family = v
                }
                // The filter chip: a toggle, its state said by the check and the accent edge as well as the fill.
                Rectangle {
                    id: readyChip
                    objectName: "templatesReadyOnly"
                    implicitWidth: chipRow.implicitWidth + 4 * dialog.u
                    implicitHeight: 7 * dialog.u
                    radius: height / 2
                    color: dialog.readyOnly ? dialog.look.selectedRow : chipHover.hovered ? dialog.look.well : "transparent"
                    border.width: 1
                    border.color: dialog.readyOnly || activeFocus ? dialog.look.accent : dialog.look.hairlineStrong
                    activeFocusOnTab: true
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: qsTr("Ready to run")
                    Accessible.checked: dialog.readyOnly
                    Keys.onSpacePressed: dialog.readyOnly = !dialog.readyOnly
                    Keys.onReturnPressed: dialog.readyOnly = !dialog.readyOnly
                    HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: dialog.readyOnly = !dialog.readyOnly }
                    ToolTip.visible: chipHover.hovered
                    ToolTip.delay: App.theme.look.tooltipDelay
                    ToolTip.text: qsTr("Only templates whose models are on this machine or on a paired host")
                    Row {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: dialog.u
                        Glyph {
                            visible: dialog.readyOnly
                            anchors.verticalCenter: parent.verticalCenter
                            look: dialog.look
                            name: "check"
                            color: dialog.look.accent
                            font.pixelSize: 12
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Ready to run")
                            color: dialog.look.text
                            font.pixelSize: dialog.look.textSize
                        }
                    }
                }
                }

                GridView {
                    id: grid
                    objectName: "templatesGrid"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: FugueScrollBar { look: dialog.look }
                    model: dialog.shown
                    keyNavigationEnabled: true
                    // As many 56u cards as fit, sharing the spare width.
                    // The scroll bar's gutter is kept free always, so cards do not reflow as it appears.
                    readonly property int room: width - (ScrollBar.vertical as FugueScrollBar).breadth
                    readonly property int columns: Math.max(1, Math.floor(room / (56 * dialog.u)))
                    cellWidth: Math.floor(room / columns)
                    cellHeight: cellWidth * 10 / 16 + 30 * dialog.u
                    Keys.onReturnPressed: if (currentIndex >= 0) dialog.openTemplate(dialog.shown[currentIndex].id)
                    Keys.onEnterPressed: if (currentIndex >= 0) dialog.openTemplate(dialog.shown[currentIndex].id)
                    Keys.onSpacePressed: if (currentIndex >= 0) dialog.showDetails(dialog.shown[currentIndex].id)
                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_I && currentIndex >= 0 && !(event.modifiers & ~Qt.ShiftModifier)) {
                            dialog.showDetails(dialog.shown[currentIndex].id)
                            event.accepted = true
                        }
                    }

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
                            TapHandler {
                                // The info chip sits over the card and shows the details instead.
                                onTapped: eventPoint => {
                                    if (infoChip.contains(infoChip.mapFromItem(card, eventPoint.position)))
                                        return
                                    grid.currentIndex = cell.index
                                    dialog.openTemplate(cell.modelData.id)
                                }
                            }

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
                                    // About this template: the details. The same chip as the models' place badge in the other
                                    // corner (theme body, strong hairline, its height), always fully shown, a bold "i": it reads on
                                    // any picture (the owner found the faint outlined one hard to see).
                                    Rectangle {
                                        id: infoChip
                                        objectName: "templateInfo-" + cell.modelData.id
                                        anchors { right: parent.right; bottom: parent.bottom; margins: dialog.u }
                                        height: 16 + dialog.u
                                        width: height
                                        radius: height / 2
                                        color: infoHover.hovered ? dialog.look.well : dialog.look.body
                                        border.width: 1
                                        border.color: infoHover.hovered ? dialog.look.accent : dialog.look.hairlineStrong
                                        Accessible.role: Accessible.Button
                                        Accessible.name: qsTr("About this template")
                                        Text {
                                            anchors.centerIn: parent
                                            text: "i"
                                            color: infoHover.hovered ? dialog.look.accent : dialog.look.text
                                            font.family: "serif"
                                            font.bold: true
                                            font.italic: false
                                            font.pixelSize: Math.round(infoChip.height * 0.72)
                                        }
                                        ToolTip.visible: infoHover.hovered
                                        ToolTip.delay: App.theme.look.tooltipDelay
                                        ToolTip.text: qsTr("About this template")
                                        // Its own press target (an exclusive grab, so the card's tap does not follow): the details, not the template.
                                        HoverHandler { id: infoHover; cursorShape: Qt.PointingHandCursor }
                                        TapHandler {
                                            gesturePolicy: TapHandler.ReleaseWithinBounds
                                            onTapped: dialog.showDetails(cell.modelData.id, cell.index)
                                        }
                                    }
                                    // Where its models are (#164): the same badges as the model pickers, on a
                                    // pill over the preview's corner so the card keeps its height.
                                    Rectangle {
                                        id: places
                                        objectName: "templatePlaces-" + cell.modelData.id
                                        anchors { left: parent.left; bottom: parent.bottom; margins: dialog.u }
                                        visible: cell.modelData.local || cell.modelData.hosts.length > 0
                                                 || cell.modelData.offlineHosts.length > 0
                                        width: placeRow.width + 2 * dialog.u
                                        height: 16 + dialog.u
                                        radius: height / 2
                                        color: dialog.look.body
                                        border.width: 1
                                        border.color: dialog.look.hairlineStrong
                                        Row {
                                            id: placeRow
                                            anchors.centerIn: parent
                                            spacing: dialog.u
                                            PlaceBadge {
                                                objectName: "templatePlaceLocal-" + cell.modelData.id
                                                visible: cell.modelData.local
                                                look: dialog.look
                                                kind: "local"
                                            }
                                            PlaceBadge {
                                                objectName: "templatePlaceHost-" + cell.modelData.id
                                                visible: cell.modelData.hosts.length > 0 || cell.modelData.offlineHosts.length > 0
                                                look: dialog.look
                                                kind: "host"
                                                hosts: cell.modelData.hosts.length > 0 ? cell.modelData.hosts : cell.modelData.offlineHosts
                                                offline: cell.modelData.hosts.length === 0
                                            }
                                        }
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
                                Text {
                                    id: sourceLine
                                    objectName: "templateSource-" + cell.modelData.id
                                    readonly property var info: cell.modelData.source
                                    readonly property string link: info.url ? info.url : ""
                                    Layout.fillWidth: true
                                    visible: !!info.name || !!info.author
                                    text: info.name && info.author ? qsTr("Based on %1 by %2").arg(info.name).arg(info.author)
                                          : info.name ? qsTr("Based on %1").arg(info.name)
                                          : qsTr("Workflow by %1").arg(info.author ? info.author : "")
                                    elide: Text.ElideRight
                                    color: link !== "" ? dialog.look.accent : dialog.look.label
                                    font.pixelSize: dialog.look.captionSize
                                    font.underline: link !== "" && sourceHover.hovered
                                    ToolTip.visible: sourceHover.hovered && (link !== "" || !!info.note)
                                    ToolTip.delay: App.theme.look.tooltipDelay
                                    ToolTip.text: [info.note ? info.note : "", link].filter(t => t !== "").join("\n")
                                    HoverHandler { id: sourceHover; cursorShape: sourceLine.link !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor }
                                    // Its own press target, as the download button: a click here opens the page, not the template.
                                    MouseArea {
                                        anchors.fill: parent
                                        enabled: sourceLine.link !== ""
                                        onClicked: Qt.openUrlExternally(sourceLine.link)
                                    }
                                }
                                RowLayout {
                                    id: statusRow
                                    readonly property var downloads: App.workflows.downloads
                                    readonly property bool downloading: downloads.busy
                                                                        && downloads.what === "template:" + cell.modelData.id
                                    readonly property bool failed: !downloads.busy
                                                                   && downloads.failedWhat === "template:" + cell.modelData.id
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 7 * dialog.u
                                    visible: cell.modelData.status !== "" || downloading || failed
                                    spacing: dialog.u
                                    Text {
                                        objectName: "templateStatus-" + cell.modelData.id
                                        Layout.fillWidth: true
                                        text: statusRow.downloading
                                              ? [statusRow.downloads.label, statusRow.downloads.progressText].filter(t => t !== "").join(" · ")
                                              : statusRow.failed ? statusRow.downloads.failure : cell.modelData.status
                                        elide: Text.ElideRight
                                        color: statusRow.downloading ? dialog.look.label : dialog.look.error
                                        font.pixelSize: dialog.look.captionSize
                                        // Which files and where each goes, or the whole failure, on hover.
                                        ToolTip.visible: statusHover.hovered && (statusRow.failed || cell.modelData.missingModels.length > 0)
                                        ToolTip.delay: App.theme.look.tooltipDelay
                                        ToolTip.text: statusRow.failed ? statusRow.downloads.failure
                                                      : cell.modelData.missingModels.map(m => m.suggest !== ""
                                                          ? qsTr("%1, into %2. You have %3, another quantization of it.").arg(m.name, m.folder, m.suggest)
                                                          : qsTr("%1, into %2").arg(m.name, m.folder)).join("\n")
                                        HoverHandler { id: statusHover }
                                    }
                                    // Its own press target: a click here does not open the template.
                                    ThemedButton {
                                        objectName: "templateDownload-" + cell.modelData.id
                                        // Destructive: Cancel stands apart, twice the row's gap before it.
                                        Layout.leftMargin: statusRow.downloading ? dialog.u : 0
                                        visible: statusRow.downloading || (cell.modelData.download !== "" && !statusRow.downloads.busy)
                                        flat: true
                                        danger: statusRow.downloading
                                        text: statusRow.downloading ? qsTr("Cancel")
                                              : cell.modelData.downloadSize !== "" ? qsTr("Download %1").arg(cell.modelData.downloadSize)
                                              : qsTr("Download")
                                        Accessible.name: statusRow.downloading ? qsTr("Cancel the download") : cell.modelData.download
                                        tip: statusRow.downloading ? "" : cell.modelData.download
                                        onClicked: statusRow.downloading ? statusRow.downloads.cancel()
                                                                         : dialog.downloadTemplate(cell.modelData.id)
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
        TemplateDetails {
            anchors.fill: parent
            visible: dialog.detail !== null
            template: dialog.detail ?? dialog.noDetail
            onOpen: dialog.openTemplate(dialog.detailId)
            onCancel: dialog.closeDetails()
        }
    }
}
