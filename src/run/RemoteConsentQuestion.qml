pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// The question a run waits on before it sends pictures, masks, audio or prompts to a service off this machine (spec 8.2, #404): which
// service (the instance's name and host) and what leaves, once per instance. Yes keeps the answer (until the instance is removed or
// its address changes) and the run goes on; No, Escape or closing it cancels the run. Reads App.runs.remoteQuestion; the answer is
// App.runs.answerRemote. Keys alone: it opens with Yes focused (Enter), Tab goes to No, Escape is No.
Question {
    id: ask
    objectName: "remoteConsentAsk"
    readonly property var services: App.runs ? (App.runs.remoteQuestion.services || []) : []
    // Open while a run waits; the answer ends the wait, which closes it (the next question, if one is queued, takes its place).
    visible: services.length > 0
    closePolicy: Popup.NoAutoClose
    onOpened: allowButton.forceActiveFocus()
    onServicesChanged: if (services.length > 0) allowButton.forceActiveFocus()
    function answer(allow) {
        App.runs.answerRemote(allow)
    }
    ColumnLayout {
        anchors.fill: parent
        Keys.onEscapePressed: ask.answer(false) // Escape is No: the run is cancelled
        spacing: 3 * App.theme.look.unit
        LayoutMirroring.enabled: Application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.bold: true
            text: qsTr("This run sends data to a service off this machine")
        }
        Repeater {
            model: ask.services
            delegate: Label {
                required property var modelData
                objectName: "remoteConsentService"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("%1 (%2) gets your %3.").arg(modelData.instance).arg(modelData.host).arg(modelData.text)
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Yes is remembered for each service until you remove it or change its address.")
        }
        QuestionAnswers {
            ThemedButton {
                objectName: "remoteConsentNo"
                text: qsTr("No, cancel the run")
                onClicked: ask.answer(false)
            }
            ThemedButton {
                id: allowButton
                objectName: "remoteConsentYes"
                text: qsTr("Yes, send it")
                highlighted: true
                onClicked: ask.answer(true)
            }
        }
    }
}
