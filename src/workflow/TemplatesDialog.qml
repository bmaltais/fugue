pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../kit"

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
    // A narrow window: the category list gives up width to the cards.
    readonly property bool compact: width < 220 * u
    // Read again each time it opens: templates and model folders change outside Fugue.
    property var templates: []
    // "" is every template, "own" the person's own, else a category id.
    property string category: ""
    // Only the templates whose models are all here or on a paired host (#164).
    property bool readyOnly: false
    // Only the apps (#387): the templates with an `app` block.
    property bool appsOnly: false
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
    background: Item {
        Shadow { target: box; level: 3; look: dialog.look }
        Item { id: box; anchors.fill: parent }
    }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: dialog.look.params.durColor * 1.4; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: dialog.look.params.durColor * 1.4; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.2, 0, 0, 1, 1, 1] }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; to: 0; duration: dialog.look.params.durColor * 0.9; easing.type: Easing.OutCubic }
    }
    onAboutToShow: {
        // The paired hosts' models arrive later (templatesChanged), or at once when already known.
        App.workflows.loadHostModels()
        templates = App.workflows.templates()
        search.text = ""
        source = ""
        family = ""
        appsOnly = false
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

    // Whether a template passes the tab, Ready to run, Apps, the search and the two drop-downs, except
    // the one named by `skip` ("source" or "family"): the counts of a drop-down say what choosing
    // each of its entries would leave.
    function passes(t, skip) {
        if (category === "own" ? !t.own : category !== "" && (t.own || t.category !== category))
            return false
        if (readyOnly && !t.ready)
            return false
        if (appsOnly && !t.app)
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

    contentItem: KitPanel {
        id: panel
        objectName: "templatesPanel"
        look: dialog.look
        tone: "sea"
        clip: true
        // Tab walks the window's controls: it is also the window's "Add a node by search" key.
        Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab
        ColumnLayout {
            anchors { fill: parent; margins: 1 }
            spacing: 0
            enabled: dialog.detail === null
            KitHero {
                id: hero
                objectName: "templatesHero"
                Layout.fillWidth: true
                look: dialog.look
                title: qsTr("Templates")
                subtitle: qsTr("Workflows to start from; the apps among them open as one simple screen.")
                tone: panel.tone
                showClose: true
                onClosed: dialog.close()
            }
        RowLayout {
            id: content
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            LayoutMirroring.enabled: dialog.rightToLeft
            LayoutMirroring.childrenInherit: true

            // The categories, and where the person's own templates go.
            ColumnLayout {
                Layout.preferredWidth: (dialog.compact ? 32 : 44) * dialog.u
                Layout.fillWidth: false
                Layout.fillHeight: true
                Layout.margins: 4 * dialog.u
                spacing: 0
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
                        radius: Math.min(dialog.look.params.cornerChip * dialog.u, 0.45 * height)
                        // The chosen category takes the window's tone, as a chip that is on does.
                        color: current ? Qt.alpha(dialog.look.tone(panel.tone).glass, 0.34) : tabHover.hovered ? dialog.look.kitChipHover : "transparent"
                        Accessible.role: Accessible.PageTab
                        Accessible.name: modelData.title
                        Accessible.selected: current
                        activeFocusOnTab: true
                        border.width: activeFocus && FocusInput.keyboard ? 2 : 0
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
                    font.pixelSize: dialog.look.labelSize
                }
            }
            Rectangle { Layout.fillHeight: true; implicitWidth: 1; color: dialog.look.hairline }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 4 * dialog.u
                spacing: 4 * dialog.u

                // The search takes what the filters leave; in a narrow window the filters wrap under it.
                Flow {
                    id: filterRow
                    Layout.fillWidth: true
                    spacing: 2 * dialog.u
                    readonly property real others: (sourceBox.visible ? sourceBox.width + spacing : 0)
                                                   + (familyBox.visible ? familyBox.width + spacing : 0) + readyChip.width + spacing
                                                   + (appsChip.visible ? appsChip.width + spacing : 0)
                KitSearch {
                    id: search
                    objectName: "templatesSearch"
                    look: dialog.look
                    width: filterRow.width - filterRow.others >= 28 * dialog.u ? filterRow.width - filterRow.others : filterRow.width
                    placeholder: qsTr("Search templates")
                    // Enter opens the first match; Down goes to the cards.
                    onDown: { grid.currentIndex = 0; grid.forceActiveFocus() }
                    onAccepted: if (dialog.shown.length > 0) dialog.openTemplate(dialog.shown[0].id)
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
                // The filter chips: toggles, raised, tinted with the window's tone while on. Apps is there only when some template is an app (#387).
                KitChip {
                    id: readyChip
                    objectName: "templatesReadyOnly"
                    look: dialog.look
                    pill: true
                    toned: true
                    text: qsTr("Ready to run")
                    glyph: dialog.readyOnly ? "check" : ""
                    tip: qsTr("Only templates whose models are on this machine or on a paired host")
                    current: dialog.readyOnly
                    Accessible.role: Accessible.CheckBox
                    Accessible.checked: dialog.readyOnly
                    onTapped: dialog.readyOnly = !dialog.readyOnly
                }
                KitChip {
                    id: appsChip
                    objectName: "templatesApps"
                    visible: dialog.templates.some(t => t.app)
                    look: dialog.look
                    pill: true
                    toned: true
                    text: qsTr("Apps")
                    glyph: dialog.appsOnly ? "check" : ""
                    tip: qsTr("Only apps: templates that open as one simple screen")
                    current: dialog.appsOnly
                    Accessible.role: Accessible.CheckBox
                    Accessible.checked: dialog.appsOnly
                    onTapped: dialog.appsOnly = !dialog.appsOnly
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
                    // Every card is the same height: the picture, a title of up to two lines, the description in what is left
                    // (up to four lines, at least two) and the foot (what is missing, then Download) at the bottom.
                    FontMetrics { id: titleMetrics; font.pixelSize: dialog.look.textSize; font.weight: Font.DemiBold }
                    FontMetrics { id: bodyMetrics; font.pixelSize: dialog.look.labelSize }
                    readonly property real lineHeight: bodyMetrics.height
                    readonly property real footHeight: 2 * bodyMetrics.height + 8 * dialog.u
                    readonly property real margin: dialog.look.params.areaGap * dialog.u / 2
                    readonly property real pad: dialog.look.params.rowPadY * dialog.u
                    cellHeight: Math.ceil((cellWidth - 2 * margin - 2 * pad) * 10 / 16 + 2 * margin + 2 * pad + 4 * dialog.u
                                          + 2 * titleMetrics.height + 4 * bodyMetrics.height + footHeight)
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

                        KitCard {
                            id: card
                            objectName: "templateCard-" + cell.modelData.id
                            anchors.fill: parent
                            anchors.margins: dialog.look.params.areaGap * dialog.u / 2
                            look: dialog.look
                            current: grid.activeFocus && grid.currentIndex === cell.index
                            Accessible.role: Accessible.Button
                            Accessible.name: cell.modelData.title
                            Accessible.description: cell.modelData.description
                            onTapped: {
                                // The info chip sits over the card and shows the details instead (its own press takes the tap).
                                grid.currentIndex = cell.index
                                dialog.openTemplate(cell.modelData.id)
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: dialog.look.params.rowPadY * dialog.u
                                spacing: dialog.u

                                KitPicture {
                                    id: picture
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: width * 10 / 16
                                    look: dialog.look
                                    source: cell.modelData.preview
                                    video: cell.modelData.video
                                    playing: card.hovered || card.current // a big catalog: only the card in hand moves
                                    radius: Math.max(2, dialog.look.params.cornerGroup * dialog.u - dialog.look.params.rowPadY * dialog.u * 0.6)
                                    // About this template: the details. A frosted round button (the kit's, adaptive ink) in the
                                    // picture's corner; its own press, so the card does not open.
                                    KitChip {
                                        id: infoChip
                                        objectName: "templateInfo-" + cell.modelData.id
                                        anchors { right: parent.right; bottom: parent.bottom; margins: dialog.u }
                                        look: dialog.look
                                        round: true
                                        frosted: true
                                        backdrop: picture.backdropItem
                                        backdropSource: cell.modelData.preview
                                        glyph: "info"
                                        tip: qsTr("About this template")
                                        Accessible.name: qsTr("About this template")
                                        onTapped: dialog.showDetails(cell.modelData.id, cell.index)
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
                                // The whole title, up to two lines (the details have the rest).
                                Text {
                                    id: titleText
                                    objectName: "templateTitle-" + cell.modelData.id
                                    Layout.fillWidth: true
                                    text: cell.modelData.title
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    color: dialog.look.text
                                    font.pixelSize: dialog.look.textSize
                                    font.weight: Font.DemiBold
                                    ToolTip.visible: titleHover.hovered && truncated
                                    ToolTip.delay: App.theme.look.tooltipDelay
                                    ToolTip.text: cell.modelData.title
                                    HoverHandler { id: titleHover }
                                }
                                // The description takes the room the title and the foot leave, as many lines as fit.
                                Item {
                                    id: descriptionSlot
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Layout.preferredHeight: 0
                                    clip: true
                                    Text {
                                        objectName: "templateDescription-" + cell.modelData.id
                                        anchors { left: parent.left; right: parent.right; top: parent.top }
                                        text: cell.modelData.description
                                        wrapMode: Text.Wrap
                                        maximumLineCount: Math.max(2, Math.min(4, Math.floor(descriptionSlot.height / grid.lineHeight)))
                                        elide: Text.ElideRight
                                        color: dialog.look.label
                                        font.pixelSize: dialog.look.labelSize
                                        // Cut off at the card's size: the whole text on hover (the details have it too).
                                        ToolTip.visible: descriptionHover.hovered && truncated
                                        ToolTip.delay: App.theme.look.tooltipDelay
                                        ToolTip.text: cell.modelData.description
                                        HoverHandler { id: descriptionHover }
                                    }
                                }
                                // The foot: what is missing, and under it the download. It is always there, so cards line up.
                                ColumnLayout {
                                    id: statusRow
                                    readonly property var downloads: App.workflows.downloads
                                    readonly property bool downloading: downloads.busy
                                                                        && downloads.what === "template:" + cell.modelData.id
                                    readonly property bool failed: !downloads.busy
                                                                   && downloads.failedWhat === "template:" + cell.modelData.id
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignBottom
                                    Layout.topMargin: dialog.u
                                    visible: cell.modelData.status !== "" || downloading || failed
                                    spacing: 0
                                    Text {
                                        objectName: "templateStatus-" + cell.modelData.id
                                        Layout.fillWidth: true
                                        text: statusRow.downloading
                                              ? [statusRow.downloads.label, statusRow.downloads.progressText].filter(t => t !== "").join(" · ")
                                              : statusRow.failed ? statusRow.downloads.failure : cell.modelData.status
                                        wrapMode: Text.Wrap
                                        maximumLineCount: 2
                                        elide: Text.ElideRight
                                        color: statusRow.downloading ? dialog.look.label : dialog.look.error
                                        font.pixelSize: dialog.look.labelSize
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
                                        Layout.topMargin: dialog.u
                                        Layout.alignment: Qt.AlignLeft
                                        visible: statusRow.downloading || (cell.modelData.download !== "" && !statusRow.downloads.busy)
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
