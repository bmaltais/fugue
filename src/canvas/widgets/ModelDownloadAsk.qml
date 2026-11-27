import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../../kit"

// The question before a catalog model is fetched (#134): what it is, its license, its size, where it comes from and where it
// will go. Nothing is fetched until Download, which says `confirmed(id)`. `info` is a model catalog row as
// WidgetEdit::catalogInfo and AppModels::catalogInfo give it. Made on each use; it destroys itself when it closes.
Question {
    id: ask
    objectName: "downloadAsk"
    required property var info
    signal confirmed(string id)
    onClosed: ask.destroy()
    ColumnLayout {
        anchors.fill: parent
        spacing: 3 * ask.look.unit
        Label {
            Layout.fillWidth: true
            objectName: "downloadAskTitle"
            text: qsTr("Download %1?").arg(ask.info.title)
            font.bold: true
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: ask.info.description !== ""
            text: ask.info.description
            color: ask.look.label
            wrapMode: Text.Wrap
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 3 * ask.look.unit
            rowSpacing: ask.look.unit
            Label { text: qsTr("License"); color: ask.look.label }
            Label {
                objectName: "downloadAskLicense"
                Layout.fillWidth: true
                text: ask.info.licenseLine
                // Not for commercial use is the one thing to notice: the error color, and the words stay.
                color: ask.info.commercial ? ask.look.text : ask.look.error
                wrapMode: Text.Wrap
            }
            Label { text: qsTr("Made by"); color: ask.look.label; visible: ask.info.author !== "" }
            Label { text: ask.info.author; Layout.fillWidth: true; wrapMode: Text.Wrap; visible: ask.info.author !== "" }
            Label { text: qsTr("Size"); color: ask.look.label }
            Label { objectName: "downloadAskSize"; text: ask.info.sizeText }
            Label { text: qsTr("From"); color: ask.look.label }
            Label { objectName: "downloadAskHost"; text: ask.info.host; Layout.fillWidth: true; elide: Text.ElideMiddle }
            Label { text: qsTr("Saved in"); color: ask.look.label }
            Label { objectName: "downloadAskFolder"; text: ask.info.folder; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("The file is kept only if its SHA-256 is the one Fugue lists for it.")
            color: ask.look.label
            font.pixelSize: ask.look.captionSize
            wrapMode: Text.Wrap
        }
        KitDialogButtons {
            ThemedButton {
                objectName: "downloadAskLicenseLink"
                dialogRole: "extra"
                flat: true
                visible: ask.info.licenseUrl !== ""
                text: qsTr("Read the license")
                onClicked: Qt.openUrlExternally(ask.info.licenseUrl)
            }
            ThemedButton { objectName: "downloadAskCancel"; dialogRole: "cancel"; text: qsTr("Cancel"); onClicked: ask.close() }
            ThemedButton {
                objectName: "downloadAskOk"
                text: qsTr("Download")
                highlighted: true
                onClicked: {
                    ask.confirmed(ask.info.id)
                    ask.close()
                }
            }
        }
    }
}
