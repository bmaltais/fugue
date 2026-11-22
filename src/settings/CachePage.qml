pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue

// The Cache page of the settings panel (spec 10.6, #141): what Fugue keeps under ~/.cache/fugue
// with a Clear for each kind (and one Clear all) and, under it, the limits that keep each kind in bounds. Clear asks
// first, in words that say what is lost. The measuring and clearing are the CacheModel's.
ColumnLayout {
    id: page
    required property var host // the SettingsPanel
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property CacheModel cache: host.model.cache
    spacing: 4 * u

    // Measured when the page is made (it is made when first shown) and after each clear.
    Component.onCompleted: cache.refresh()

    SettingsParts.Setting {
        title: qsTr("Cache")
        note: qsTr("Copies Fugue can make again, kept to speed things up. Your saved pictures and the inputs folder are never in this list.")
        // The total, and the page's one big action beside it: on the left, away from the
        // per-kind Clear… at the right edge of the rows below, so the two are not mistaken.
        RowLayout {
            Layout.fillWidth: true
            spacing: 4 * page.u
            Text {
                objectName: "cacheTotal"
                text: page.cache.busy && page.cache.kinds.length === 0 ? qsTr("Measuring…") : qsTr("%1 in all").arg(page.cache.total)
                color: page.look.label
                font.pixelSize: page.look.labelSize
            }
            SettingsParts.SmallButton {
                objectName: "cacheClearAll"
                text: qsTr("Clear all…")
                enabled: page.cache.canClearAll && !page.cache.busy
                onClicked: page.cache.ask("all")
            }
        }
    }

    SizedList {
        Layout.fillWidth: true
        name: "cache"
        model: page.cache.kinds.map(k => ({
            title: k.title, detail: k.detail, size: k.size, bytes: k.bytes,
            actionText: qsTr("Clear…"), actionEnabled: k.canClear && !page.cache.busy
        }))
        onActionClicked: index => page.cache.ask(page.cache.kinds[index].kind)
    }

    // The question: what will be lost, and the person says yes or no.
    Rectangle {
        id: ask
        objectName: "cacheQuestion"
        visible: page.cache.question.kind !== undefined
        readonly property var q: page.cache.question
        Layout.fillWidth: true
        implicitHeight: askColumn.implicitHeight + 6 * page.u
        radius: page.look.smallCorner
        color: page.look.well
        border.width: 2
        border.color: page.look.accent
        ColumnLayout {
            id: askColumn
            x: 3 * page.u
            y: 3 * page.u
            width: ask.width - 6 * page.u
            spacing: page.u
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: page.look.text
                font.pixelSize: page.look.textSize
                font.weight: Font.DemiBold
                text: qsTr("Clear %1 of %2?").arg(ask.q.size === undefined || ask.q.size === "" ? "…" : ask.q.size)
                          .arg(ask.q.title === undefined ? "" : ask.q.title.toLowerCase())
            }
            Text {
                objectName: "cacheQuestionDetail"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: page.look.label
                font.pixelSize: page.look.labelSize
                text: ask.q.what === undefined ? "" : ask.q.what
            }
            Row {
                spacing: page.u
                SettingsParts.SmallButton {
                    objectName: "cacheYes"
                    highlighted: true
                    text: qsTr("Clear")
                    onClicked: page.cache.answer(true)
                }
                SettingsParts.SmallButton {
                    objectName: "cacheNo"
                    text: qsTr("Cancel")
                    onClicked: page.cache.answer(false)
                }
            }
        }
    }
    Text {
        objectName: "cacheMessage"
        visible: text !== "" && !ask.visible
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: page.cache.message
        color: page.look.label
        font.pixelSize: page.look.labelSize
    }

    // Limits: each kept within by the oldest going first, at start, when idle and before a run.
    SettingsParts.Setting {
        title: qsTr("Results limit")
        note: qsTr("Over it, the results used longest ago go first. Frozen results stay.")
        Choices {
            Layout.fillWidth: true
            name: "cacheResults"
            current: page.host.model.cacheResultsGb
            options: [{value: 2, text: qsTr("2 GB")}, {value: 5, text: qsTr("5 GB")}, {value: 10, text: qsTr("10 GB")},
                      {value: 20, text: qsTr("20 GB")}, {value: 50, text: qsTr("50 GB")}, {value: 0, text: qsTr("No limit")}]
            onPicked: value => { page.host.apply(page.host.model.set("cache_results_gb", value)); page.cache.refresh() }
        }
    }
    SettingsParts.Setting {
        title: qsTr("Image previews limit")
        note: qsTr("The small pictures nodes show. Over it, or unseen for a week, the oldest go.")
        Choices {
            Layout.fillWidth: true
            name: "cacheThumbnails"
            current: page.host.model.cacheThumbnailsGb
            options: [{value: 0.25, text: qsTr("250 MB")}, {value: 0.5, text: qsTr("500 MB")}, {value: 1, text: qsTr("1 GB")},
                      {value: 2, text: qsTr("2 GB")}, {value: 0, text: qsTr("No limit")}]
            onPicked: value => { page.host.apply(page.host.model.set("cache_thumbnails_gb", value)); page.cache.refresh() }
        }
    }
    SettingsParts.Setting {
        title: qsTr("Runs kept")
        note: qsTr("Runs go a week after they end, except the newest ones, which stay whatever their age.")
        Choices {
            Layout.fillWidth: true
            name: "runsKept"
            current: page.host.model.runsKept
            options: [{value: 0, text: qsTr("None")}, {value: 5, text: "5"}, {value: 20, text: "20"},
                      {value: 50, text: "50"}, {value: 200, text: "200"}]
            onPicked: value => page.host.apply(page.host.model.set("runs_kept", value))
        }
    }
}
