pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas/widgets"
import "../kit"

// A model input: a KitDropdown (raised: it is clicked; owner, 2026-11-25) with the model's name that opens the model menu (WidgetMenu, searched by word start), the same menu
// rows as a node's model picker: the models the providers offer, the files in the model folders with their size and the
// Recommended one marked (#370, with its reason), and the catalog's models not on disk under Download, which asks first
// (ModelDownloadAsk: license, size, source, folder) and fetches through the window's one download line. AppView holds the
// lists and the value; a file whose hash is not known is read first, and a download is the value once it has arrived: the
// dropdown says so meanwhile.
KitDropdown {
    id: root
    required property var field
    required property AppView appView
    required look
    property var current: field.value
    property string notice
    readonly property bool waiting: appView.waitingModels.includes(field.key)
    readonly property string shown: notice !== "" ? notice : waiting ? qsTr("Getting the model ready…")
                                    : current && current.name ? current.name : qsTr("Choose a model…")

    objectName: "appInput-" + field.key
    text: shown
    ink: notice !== "" ? look.error : waiting || !(current && current.name) ? look.label : look.text
    Accessible.name: field.label
    onTapped: openMenu()

    Connections {
        target: root.appView
        function onValueChanged(key) {
            if (key === root.field.key)
                root.current = root.appView.valueOf(key)
        }
    }

    function sizeText(bytes) {
        const gb = bytes / 1e9
        return gb >= 1 ? qsTr("%1 GB").arg(gb.toFixed(1)) : qsTr("%1 MB").arg(Math.max(1, Math.round(bytes / 1e6)))
    }
    // The menu's rows from AppView.models(key): models first (by folder or family heading), then the downloads.
    function entries() {
        const key = field.key
        const listed = appView.models(key)
        const have = listed.filter(m => m.catalog !== true)
        const downloadable = listed.filter(m => m.catalog === true)
        const name = current ? current.name : undefined
        const rows = []
        let group = null
        for (const m of have) {
            const heading = m.path !== undefined ? (m.family ? m.family : m.folder) : m.provider
            if (heading !== group) {
                group = heading
                if (heading)
                    rows.push({ text: heading, heading: true, enabled: false })
            }
            if (m.path !== undefined) {
                rows.push({ text: m.title, detail: (m.recommended === true ? qsTr("Recommended") + "  " : "") + sizeText(m.size),
                            note: m.engineNote ? m.engineNote : (m.recommended === true ? m.recommendedWhy : ""), value: { disk: m.path }, place: "local",
                            checked: m.name === name })
            } else {
                const status = m.status === "ready" ? "" : m.status === "loading" ? qsTr("loading") : qsTr("not running")
                rows.push({ text: m.title, detail: status, note: m.note, value: { handle: m.handle }, place: "local",
                            checked: m.name === name })
            }
        }
        if (downloadable.length > 0)
            rows.push({ text: qsTr("Download"), heading: true, enabled: false })
        for (const m of downloadable)
            rows.push({ text: m.title, detail: sizeText(m.size), note: m.detail + "\n" + m.note, value: { download: m.id } })
        if (appView.modelsLoading(key))
            rows.push({ text: qsTr("Looking for models…"), enabled: false })
        else if (listed.length === 0)
            rows.push({ text: appView.modelsEmptyText(key), enabled: false })
        return rows
    }
    function openMenu() {
        appView.requestModels(field.key)
        const at = root.mapToItem(Overlay.overlay, 0, root.height)
        const end = root.mapToItem(Overlay.overlay, root.width, 0)
        const menu = menuComponent.createObject(root, { wantX: at.x, wantY: at.y, anchorWidth: end.x - at.x, elide: Text.ElideMiddle,
                                                         anchorHeight: root.height, entries: root.entries() }) as WidgetMenu
        menu.open()
    }
    function picked(value) {
        if (value.disk !== undefined)
            appView.pickModelFile(field.key, value.disk)
        else if (value.handle !== undefined)
            appView.pickModel(field.key, value.handle)
        else if (value.download !== undefined) {
            const info = appView.catalogInfo(field.key, value.download)
            if (info.id !== undefined)
                (askComponent.createObject(root, { info: info }) as ModelDownloadAsk).open()
        }
    }
    Component {
        id: menuComponent
        WidgetMenu {
            id: m
            look: root.look
            parent: Overlay.overlay
            searchable: true
            wordStart: false
            nothingMatches: qsTr("No model matches “%1”")
            onPicked: value => root.picked(value)
            onClosed: m.destroy()
            Connections {
                target: root.appView
                function onModelsChanged() { m.entries = root.entries() }
            }
        }
    }
    Component {
        id: askComponent
        ModelDownloadAsk {
            parent: Overlay.overlay
            onConfirmed: id => root.notice = root.appView.downloadModel(root.field.key, id)
        }
    }
}
