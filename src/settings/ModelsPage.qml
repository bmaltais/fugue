pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue

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

    // Read when the page is made (when first shown).
    Component.onCompleted: files = host.model.modelFiles()

    SettingsParts.Setting {
        title: qsTr("Models")
        note: qsTr("The model files Fugue found, by family. Recommended marks the file of a model that fits this computer, with the best quality at the best speed here. Folders are set on the Folders page.")
        Choices {
            Layout.fillWidth: true
            visible: page.families.length > 0
            name: "modelFamily"
            current: page.family
            options: [{value: "", text: qsTr("All")}].concat(page.families.map(f => ({value: f, text: f})))
            onPicked: value => page.family = value
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
        model: page.shown.map(f => ({
            title: f.name,
            detail: (f.family !== "" ? f.family : qsTr("Other")) + (f.recommended ? " · " + f.reason : ""),
            size: (f.size / 1e9 >= 1) ? qsTr("%1 GB").arg((f.size / 1e9).toFixed(1)) : qsTr("%1 MB").arg(Math.max(1, Math.round(f.size / 1e6))),
            bytes: f.size, actionText: "", actionEnabled: false
        }))
    }
}
