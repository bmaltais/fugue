pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue

// What groups and Frames ask (spec 5): whether Make group leaves the selected sinks outside
// (5.5), a Frame's new title (5.1), and the interface editor of the group shown (5.4): its
// name, its exposed inputs (name, label, section, range, the inner inputs they drive, order)
// and outputs. Every change is one edit of App.canvasModel; a refusal shows its reason.
Item {
    id: root

    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    readonly property CanvasLook look: App.theme.look
    readonly property CanvasModel model: App.canvasModel

    property alias sinksQuestion: sinksAsk
    property alias frameRename: frameAsk
    property alias interfaceEditor: editor

    Connections {
        target: root.model
        function onMakeGroupAsked(sinks) {
            sinksAsk.sinks = sinks
            sinksAsk.open()
        }
        function onFrameRenameRequested(frame) {
            const found = root.model.frames.find(f => f.id === frame)
            frameAsk.frame = frame
            frameTitle.text = found ? found.title : ""
            frameAsk.open()
        }
        function onInterfaceEditorRequested() {
            if (root.model.inGroup)
                editor.open()
        }
    }

    // Why a group edit was refused, when the editor is not there to say it.
    Rectangle {
        objectName: "groupNotice"
        visible: root.model.groupProblem !== "" && !editor.visible
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: visible ? noticeRow.implicitHeight + 16 : 0
        color: root.look.popup
        border.color: root.look.hairline
        RowLayout {
            id: noticeRow
            anchors { fill: parent; margins: 8 }
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: root.look.text
                text: root.model.groupProblem
            }
            ToolButton {
                text: "\u00d7"
                focusPolicy: Qt.NoFocus
                Accessible.name: qsTr("Dismiss")
                onClicked: root.model.dismissGroupProblem()
            }
        }
    }

    component Question: Popup {
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        padding: 6 * root.look.unit
        width: Math.min(root.width - 12 * root.look.unit, 520)
        Overlay.modal: Rectangle { color: root.look.scrim }
        background: PopupSurface { look: root.look }
    }

    Question {
        id: sinksAsk
        objectName: "groupSinksAsk"
        property var sinks: []
        ColumnLayout {
            anchors.fill: parent
            spacing: 16
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("A group never writes files or shows previews from inside. Leave %1 outside the group, wired to its output?")
                          .arg(sinksAsk.sinks.join(", "))
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button { text: qsTr("Cancel"); onClicked: sinksAsk.close() }
                Button {
                    objectName: "groupSinksInside"
                    text: qsTr("Put inside")
                    onClicked: { sinksAsk.close(); root.model.makeGroup(false) }
                }
                Button {
                    objectName: "groupSinksOutside"
                    text: qsTr("Leave outside")
                    highlighted: true
                    onClicked: { sinksAsk.close(); root.model.makeGroup(true) }
                }
            }
        }
    }

    Question {
        id: frameAsk
        objectName: "frameRenameAsk"
        property string frame: ""
        onOpened: frameTitle.forceActiveFocus()
        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label { text: qsTr("Frame title") }
            TextField {
                id: frameTitle
                objectName: "frameTitle"
                Layout.fillWidth: true
                onAccepted: frameOk.clicked()
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button { text: qsTr("Cancel"); onClicked: frameAsk.close() }
                Button {
                    id: frameOk
                    objectName: "frameRenameOk"
                    text: qsTr("Rename")
                    highlighted: true
                    onClicked: { frameAsk.close(); root.model.renameFrame(frameAsk.frame, frameTitle.text) }
                }
            }
        }
    }

    // The interface editor (5.4). Its rows are made from groupInterface() again after each edit.
    Question {
        id: editor
        objectName: "groupInterfaceEditor"
        width: Math.min(root.width - 12 * root.look.unit, 760)
        height: Math.min(root.height - 12 * root.look.unit, 640)
        property var face: ({})
        function reload() { face = root.model.groupInterface() }
        onOpened: reload()
        Connections {
            target: root.model
            enabled: editor.visible
            function onEdited() { editor.reload() }
            function onScopeChanged() { if (!root.model.inGroup) editor.close(); else editor.reload() }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Group name") }
                TextField {
                    objectName: "groupName"
                    Layout.fillWidth: true
                    text: editor.face.name || ""
                    onEditingFinished: if (text !== editor.face.name) root.model.renameGroup(text)
                }
                Label {
                    color: root.look.label
                    text: (editor.face.id || "") + " " + (editor.face.version || "")
                }
            }
            Label {
                visible: root.model.groupProblem !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: root.look.error
                text: root.model.groupProblem
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                ColumnLayout {
                    width: editor.availableWidth
                    spacing: 8

                    Label {
                        font.bold: true
                        text: qsTr("Inputs")
                    }
                    Label {
                        visible: (editor.face.inputs || []).length === 0
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: root.look.label
                        text: qsTr("Right-click an inner input and choose Expose on the group to add one.")
                    }
                    Repeater {
                        model: editor.face.inputs || []
                        delegate: Rectangle {
                            id: inputRow
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            implicitHeight: inputColumn.implicitHeight + 4 * root.look.unit
                            radius: root.look.smallCorner
                            color: root.look.well
                            function change(key, value) {
                                const next = {name: modelData.name, label: modelData.label, section: modelData.section,
                                              min: modelData.min, max: modelData.max}
                                next[key] = value
                                root.model.editExposedInput(modelData.name, next)
                            }
                            ColumnLayout {
                                id: inputColumn
                                x: 2 * root.look.unit
                                y: 2 * root.look.unit
                                width: parent.width - 4 * root.look.unit
                                RowLayout {
                                    Layout.fillWidth: true
                                    TextField {
                                        objectName: "exposedName" + inputRow.index
                                        Layout.preferredWidth: 140
                                        text: inputRow.modelData.name
                                        placeholderText: qsTr("name")
                                        onEditingFinished: if (text !== inputRow.modelData.name) inputRow.change("name", text)
                                    }
                                    TextField {
                                        Layout.fillWidth: true
                                        text: inputRow.modelData.label
                                        placeholderText: qsTr("Label")
                                        onEditingFinished: if (text !== inputRow.modelData.label) inputRow.change("label", text)
                                    }
                                    TextField {
                                        objectName: "exposedSection" + inputRow.index
                                        Layout.preferredWidth: 120
                                        text: inputRow.modelData.section
                                        placeholderText: qsTr("Section")
                                        onEditingFinished: if (text !== inputRow.modelData.section) inputRow.change("section", text)
                                    }
                                    Label { color: root.look.label; text: inputRow.modelData.type }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    visible: inputRow.modelData.type === "number"
                                    Label { text: qsTr("Range") }
                                    TextField {
                                        Layout.preferredWidth: 90
                                        text: inputRow.modelData.min === undefined ? "" : String(inputRow.modelData.min)
                                        placeholderText: qsTr("min")
                                        validator: DoubleValidator {}
                                        onEditingFinished: inputRow.change("min", text)
                                    }
                                    TextField {
                                        Layout.preferredWidth: 90
                                        text: inputRow.modelData.max === undefined ? "" : String(inputRow.modelData.max)
                                        placeholderText: qsTr("max")
                                        validator: DoubleValidator {}
                                        onEditingFinished: inputRow.change("max", text)
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.Wrap
                                        color: root.look.label
                                        text: qsTr("Drives %1").arg(inputRow.modelData.targets.join(", "))
                                    }
                                    ComboBox {
                                        id: alsoDrive
                                        readonly property var candidates: editor.visible ? root.model.exposeCandidates(inputRow.modelData.name) : []
                                        visible: candidates.length > 0
                                        model: [qsTr("Also drive…")].concat(candidates.map(c => c.title))
                                        onActivated: index => {
                                            if (index > 0) {
                                                const c = candidates[index - 1]
                                                root.model.exposeInput(c.node, c.input, inputRow.modelData.name)
                                            }
                                            currentIndex = 0
                                        }
                                    }
                                    ToolButton {
                                        text: "↑"
                                        enabled: inputRow.index > 0
                                        Accessible.name: qsTr("Move up")
                                        onClicked: root.model.moveExposed(false, inputRow.modelData.name, inputRow.index - 1)
                                    }
                                    ToolButton {
                                        text: "↓"
                                        enabled: inputRow.index < (editor.face.inputs || []).length - 1
                                        Accessible.name: qsTr("Move down")
                                        onClicked: root.model.moveExposed(false, inputRow.modelData.name, inputRow.index + 1)
                                    }
                                    ToolButton {
                                        text: "×"
                                        Accessible.name: qsTr("Stop exposing")
                                        onClicked: root.model.removeExposedInput(inputRow.modelData.name)
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.topMargin: 8
                        font.bold: true
                        text: qsTr("Outputs")
                    }
                    Repeater {
                        model: editor.face.outputs || []
                        delegate: RowLayout {
                            id: outputRow
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            TextField {
                                Layout.preferredWidth: 140
                                text: outputRow.modelData.name
                                onEditingFinished: if (text !== outputRow.modelData.name)
                                                       root.model.renameExposedOutput(outputRow.modelData.name, text)
                            }
                            Label {
                                Layout.fillWidth: true
                                color: root.look.label
                                text: qsTr("from %1").arg(outputRow.modelData.from) + "  " + outputRow.modelData.type
                            }
                            ToolButton {
                                text: "↑"
                                enabled: outputRow.index > 0
                                Accessible.name: qsTr("Move up")
                                onClicked: root.model.moveExposed(true, outputRow.modelData.name, outputRow.index - 1)
                            }
                            ToolButton {
                                text: "↓"
                                enabled: outputRow.index < (editor.face.outputs || []).length - 1
                                Accessible.name: qsTr("Move down")
                                onClicked: root.model.moveExposed(true, outputRow.modelData.name, outputRow.index + 1)
                            }
                            ToolButton {
                                text: "×"
                                Accessible.name: qsTr("Stop exposing")
                                onClicked: root.model.unexposeOutput(outputRow.modelData.name)
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    objectName: "groupInterfaceClose"
                    text: qsTr("Done")
                    highlighted: true
                    onClicked: editor.close()
                }
            }
        }
    }
}
