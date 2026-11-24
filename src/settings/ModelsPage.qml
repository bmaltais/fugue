pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue
import "../canvas"

// The Models page of the settings panel (#370): the model files Fugue found, grouped by family, with a
// family filter, and the Recommended file of each model marked with its reason (engine/model_recommend.h).
// Read-only: the folders are the Folders page's, files are never changed here.
ColumnLayout {
    id: page
    required property var host // the SettingsPanel
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    property var files: []
    property string family: "" // "" shows every family
    readonly property var families: {
        const names = []
        for (const f of files)
            if (f.family !== "" && !names.includes(f.family))
                names.push(f.family)
        return names.sort()
    }
    readonly property var shown: files.filter(f => family === "" || f.family === family)
    spacing: 4 * u

    // The reason without its "Recommended: " lead: the badge says that.
    function reasonOf(f) {
        const text = f.reason.replace(/^Recommended: /, "")
        return text.charAt(0).toUpperCase() + text.slice(1)
    }

    // Read when the page is made (when first shown).
    Component.onCompleted: files = host.model.modelFiles()

    SettingsParts.Setting {
        title: qsTr("Models")
        note: qsTr("The model files Fugue found, by family. A Recommended badge marks the file of a model that fits this computer, with the best quality at the best speed here. Folders are set on the Folders page.")
        RowLayout {
            visible: page.families.length > 0
            spacing: 2 * page.u
            Text {
                text: qsTr("Family")
                color: page.look.label
                font.pixelSize: page.look.labelSize
            }
            ThemedDropDown {
                objectName: "modelFamily"
                options: [{value: "", text: qsTr("All families")}].concat(page.families.map(f => ({value: f, text: f})))
                value: page.family
                label: qsTr("Family")
                searchable: true
                onPicked: v => page.family = v
            }
        }
    }

    Text {
        objectName: "modelsEmpty"
        visible: page.shown.length === 0
        text: qsTr("No model files found.")
        color: page.look.label
        font.pixelSize: page.look.labelSize
    }

    SizedList {
        Layout.fillWidth: true
        name: "models"
        bar: false // a file's size says it; a bar of its share of the biggest means nothing here
        model: page.shown.map(f => ({
            title: f.name,
            detail: (f.family !== "" ? f.family : qsTr("Other")) + (f.recommended ? " · " + page.reasonOf(f) : ""),
            badge: f.recommended ? qsTr("Recommended") : "",
            size: (f.size / 1e9 >= 1) ? qsTr("%1 GB").arg((f.size / 1e9).toFixed(1)) : qsTr("%1 MB").arg(Math.max(1, Math.round(f.size / 1e6))),
            bytes: f.size, actionText: "", actionEnabled: false
        }))
    }
}
