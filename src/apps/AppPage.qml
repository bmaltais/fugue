pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// The App view (#385, epic #397, spec 12.17): a workflow as one simple screen. Inputs at the start, the output beside
// them, nothing about how it is made. AppView (apps/app_view.h) holds all the logic; this draws it.
//
//   header: the app's title and description; at the end, Open in Graph (the one way out to the nodes) and, quietly, Add to
//           launcher (#398). In an app's own window (`standalone`) there is no graph to open: its quiet way out is Open in
//           Fugue, the full window in a process of its own
//   form:   the inputs (AppInput), the Advanced ones folded; under them Run, the screen's one primary control, with Cancel
//           in a chunk of its own while a run goes, and one line of progress
//   output: what the last run made (a picture is drawn to fit, a double click opens it large; a video or sound plays),
//           an empty state before it, and after a failed run one line of what happened with a quiet Details for the rest
//
// Rules met: docs/ui-ergonomics.md, "App view" row.
Rectangle {
    id: page
    objectName: "appPage"
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property AppView appView: App.appView
    readonly property AppLauncher launcher: App.appLauncher
    readonly property bool launchable: launcher !== null && launcher.available
    // The app's own window (apps/AppWindow.qml): the page is the whole window and has no graph behind it.
    property bool standalone: false
    property bool detailsOpen: false
    property bool advancedOpen: false
    readonly property var visibleFields: appView.fields.filter(f => !f.advanced)
    readonly property var advancedFields: appView.fields.filter(f => f.advanced)

    color: look.popup

    // The app being another one: Details and Advanced start closed again.
    Connections {
        target: page.appView
        function onAppChanged() {
            page.detailsOpen = false
            page.advancedOpen = false
        }
    }

    ColumnLayout {
        anchors { fill: parent; margins: 4 * page.u }
        spacing: 3 * page.u

        RowLayout {
            Layout.fillWidth: true
            spacing: 3 * page.u
            ColumnLayout {
                Layout.fillWidth: true
                spacing: page.u
                Text {
                    objectName: "appTitle"
                    Layout.fillWidth: true
                    text: page.appView.title
                    color: page.look.text
                    font.pixelSize: page.look.overlayTitleSize
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: page.appView.description
                    color: page.look.label
                    font.pixelSize: page.look.labelSize
                    wrapMode: Text.Wrap
                }
            }
            RowLayout {
                spacing: 2 * page.u
                ViewButton {
                    objectName: "appLauncherToggle"
                    visible: page.launchable
                    look: page.look
                    text: page.launcher && page.launcher.listed ? qsTr("Remove from launcher") : qsTr("Add to launcher")
                    glyph: "pin"
                    flat: true
                    tip: page.launcher && page.launcher.listed ? qsTr("Take this app out of the desktop launcher")
                                                               : qsTr("Start this app from the desktop launcher, in a window of its own")
                    onTapped: page.launcher.listed ? page.launcher.remove() : page.launcher.add()
                }
                ViewButton {
                    objectName: "appOpenInFugue"
                    visible: page.standalone && page.launchable
                    look: page.look
                    text: qsTr("Open in Fugue")
                    glyph: "workflow"
                    flat: true
                    tip: qsTr("Open the workflow behind this app in the full Fugue window")
                    onTapped: page.launcher.openInFugue()
                }
                ViewButton {
                    objectName: "appOpenInGraph"
                    visible: !page.standalone
                    look: page.look
                    text: qsTr("Open in Graph")
                    glyph: "workflow"
                    bordered: true
                    tip: qsTr("Show the workflow behind this app")
                    onTapped: page.appView.openInGraph()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 4 * page.u

            // The form.
            ColumnLayout {
                Layout.preferredWidth: 52 * page.u
                Layout.maximumWidth: 52 * page.u
                Layout.fillHeight: true
                spacing: 3 * page.u

                ScrollView {
                    id: scroll
                    objectName: "appForm"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true
                    ColumnLayout {
                        width: scroll.availableWidth
                        spacing: 3 * page.u
                        Repeater {
                            model: page.visibleFields
                            delegate: AppInput {
                                required property var modelData
                                Layout.fillWidth: true
                                field: modelData
                                onEdited: (key, value) => page.appView.setValue(key, value)
                            }
                        }
                        ViewButton {
                            visible: page.advancedFields.length > 0
                            objectName: "appAdvanced"
                            look: page.look
                            text: qsTr("Advanced")
                            flat: true
                            trailingGlyph: page.advancedOpen ? "chevron-up" : "chevron-down"
                            checkable: true
                            current: page.advancedOpen
                            accented: false
                            onTapped: page.advancedOpen = !page.advancedOpen
                        }
                        Repeater {
                            model: page.advancedOpen ? page.advancedFields : []
                            delegate: AppInput {
                                required property var modelData
                                Layout.fillWidth: true
                                field: modelData
                                onEdited: (key, value) => page.appView.setValue(key, value)
                            }
                        }
                    }
                }

                Text {
                    objectName: "appProblem"
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: page.appView.problem
                    color: page.look.error
                    font.pixelSize: page.look.labelSize
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 3 * page.u
                    ViewButton {
                        objectName: "appRun"
                        Layout.fillWidth: true
                        look: page.look
                        text: qsTr("Run")
                        glyph: "play"
                        primary: true
                        enabled: page.appView.canRun
                        onTapped: page.appView.run()
                    }
                    ViewButton {
                        objectName: "appCancel"
                        look: page.look
                        text: qsTr("Cancel")
                        danger: true
                        visible: page.appView.busy
                        onTapped: page.appView.cancel()
                    }
                }
                // One line of progress while a run goes: where it is, and a 2px bar.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: page.appView.busy
                    spacing: page.u
                    Text {
                        objectName: "appProgressText"
                        Layout.fillWidth: true
                        text: page.appView.status === "waiting" ? qsTr("Waiting for its turn…")
                            : page.appView.progress >= 0 ? qsTr("Working… %1%").arg(Math.round(page.appView.progress * 100))
                            : qsTr("Working…")
                        color: page.look.label
                        font.pixelSize: page.look.captionSize
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 2
                        color: page.look.well
                        Rectangle {
                            width: parent.width * Math.max(0, page.appView.progress)
                            height: parent.height
                            color: page.look.accent
                        }
                    }
                }
            }

            // The output.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 2 * page.u

                RowLayout {
                    Layout.fillWidth: true
                    visible: page.appView.status === "failed"
                    spacing: 2 * page.u
                    Text {
                        objectName: "appError"
                        Layout.fillWidth: true
                        text: page.appView.error
                        color: page.look.error
                        font.pixelSize: page.look.textSize
                        wrapMode: Text.Wrap
                    }
                    ViewButton {
                        objectName: "appDetails"
                        look: page.look
                        text: qsTr("Details")
                        flat: true
                        trailingGlyph: page.detailsOpen ? "chevron-up" : "chevron-down"
                        checkable: true
                        current: page.detailsOpen
                        accented: false
                        visible: page.appView.details.length > 1 || (page.appView.details.length === 1 && page.appView.details[0].hint !== "")
                        onTapped: page.detailsOpen = !page.detailsOpen
                    }
                }
                Column {
                    objectName: "appDetailsList"
                    Layout.fillWidth: true
                    visible: page.detailsOpen && page.appView.status === "failed"
                    spacing: page.u
                    Repeater {
                        model: page.appView.details
                        delegate: Text {
                            required property var modelData
                            width: parent.width
                            text: modelData.hint !== "" ? modelData.message + " " + modelData.hint : modelData.message
                            color: page.look.label
                            font.pixelSize: page.look.labelSize
                            wrapMode: Text.Wrap
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: page.look.smallCorner
                    color: page.look.well
                    Text {
                        objectName: "appEmpty"
                        anchors.centerIn: parent
                        visible: page.appView.results.length === 0
                        text: page.appView.busy ? "" : qsTr("The result appears here.")
                        color: page.look.label
                        font.pixelSize: page.look.textSize
                    }
                    ListView {
                        id: resultList
                        objectName: "appResults"
                        anchors { fill: parent; margins: 2 * page.u }
                        spacing: 2 * page.u
                        clip: true
                        orientation: ListView.Vertical
                        model: page.appView.results
                        delegate: ColumnLayout {
                            id: tile
                            required property var modelData
                            width: resultList.width
                            height: Math.max(resultList.height / Math.max(1, resultList.count), 24 * page.u)
                            spacing: page.u
                            Text {
                                visible: resultList.count > 1
                                text: tile.modelData.label
                                color: page.look.label
                                font.pixelSize: page.look.labelSize
                            }
                            Image {
                                visible: tile.modelData.kind === "image"
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                source: tile.modelData.kind === "image" ? tile.modelData.url : ""
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                cache: false
                                mipmap: true
                                Accessible.role: Accessible.Graphic
                                Accessible.name: tile.modelData.label
                                TapHandler {
                                    acceptedButtons: Qt.LeftButton
                                    onDoubleTapped: resultDialog.openFor(tile.modelData)
                                }
                            }
                            Loader {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                active: tile.modelData.kind === "video" || tile.modelData.kind === "audio"
                                sourceComponent: MediaView {
                                    look: page.look
                                    source: tile.modelData.url
                                    kind: tile.modelData.kind
                                    label: tile.modelData.text
                                }
                            }
                            Text {
                                visible: tile.modelData.kind === "text"
                                Layout.fillWidth: true
                                text: tile.modelData.text
                                color: page.look.text
                                font.pixelSize: page.look.textSize
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
        }
    }
    AppResultDialog { id: resultDialog }
}
