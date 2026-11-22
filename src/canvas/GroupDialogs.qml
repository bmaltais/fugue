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
    property alias rename: renameAsk
    property alias interfaceEditor: editor

    Connections {
        target: root.model
        function onMakeGroupAsked(sinks) {
            sinksAsk.sinks = sinks
            sinksAsk.open()
        }
        function onLibraryRenameOffered(libraryId, name) {
            libraryAsk.libraryId = libraryId
            libraryAsk.name = name
            libraryAsk.open()
        }
        function onPortRenameRequested(node, port, output) {
            const found = root.model.portToRename(node, port, output)
            const prompt = output ? qsTr("Output name on the group") : qsTr("Input name on the group")
            renameAsk.ask(node, port, output, prompt, found.label || "")
        }
        function onGroupUpdateAsked(node, changes) {
            updateAsk.node = node
            updateAsk.changes = changes
            updateAsk.major = false
            updateAsk.open()
        }
        // A node's newer major version (3.6, #118): the same question, said for one node.
        function onNodeMoveAsked(node, changes) {
            updateAsk.node = node
            updateAsk.changes = changes
            updateAsk.major = true
            updateAsk.open()
        }
        function onInterfaceEditorRequested() {
            if (root.model.inGroup)
                editor.open()
        }
    }

    // Why a group edit was refused (when the editor is not there to say it), or where a group
    // was saved.
    Rectangle {
        objectName: "groupNotice"
        visible: (root.model.groupProblem !== "" && !editor.visible) || root.model.groupNotice !== ""
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: visible ? noticeRow.implicitHeight + 4 * root.look.unit : 0
        color: root.look.popup
        border.color: root.look.hairline
        RowLayout {
            id: noticeRow
            anchors { fill: parent; margins: 2 * root.look.unit }
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: root.look.text
                text: root.model.groupProblem !== "" ? root.model.groupProblem : root.model.groupNotice
            }
            GlyphButton {
                glyph: "x"
                focusPolicy: Qt.NoFocus
                label: qsTr("Dismiss")
                onClicked: { root.model.dismissGroupProblem(); root.model.dismissGroupNotice() }
            }
        }
    }

    // A small button with a Lucide glyph (docs/canvas-look.md 16) and its name for screen readers.
    component GlyphButton: ToolButton {
        id: glyphButton
        property string glyph
        property string label
        Accessible.name: label
        ToolTip.text: label
        ToolTip.visible: hovered
        ToolTip.delay: App.theme.look.tooltipDelay
        contentItem: Glyph {
            look: root.look
            name: glyphButton.glyph
            color: glyphButton.enabled ? root.look.text : root.look.label
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }


    Question {
        id: sinksAsk
        objectName: "groupSinksAsk"
        property var sinks: []
        ColumnLayout {
            anchors.fill: parent
            spacing: 4 * root.look.unit
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
                ThemedButton { text: qsTr("Cancel"); onClicked: sinksAsk.close() }
                ThemedButton {
                    objectName: "groupSinksInside"
                    text: qsTr("Put inside")
                    onClicked: { sinksAsk.close(); root.model.makeGroup(false) }
                }
                ThemedButton {
                    objectName: "groupSinksOutside"
                    text: qsTr("Leave outside")
                    highlighted: true
                    onClicked: { sinksAsk.close(); root.model.makeGroup(true) }
                }
            }
        }
    }

    // An update from the library (5.3) says what it changes first (3.6); every instance moves.
    // The same question moves one node to a newer major of its type (#118).
    Question {
        id: updateAsk
        objectName: "groupUpdateAsk"
        property string node: ""
        property var changes: ({})
        property bool major: false // one node's newer major, not a group's library update
        function line(label, names) { return names && names.length > 0 ? label.arg(names.join(", ")) : "" }
        ColumnLayout {
            anchors.fill: parent
            spacing: 3 * root.look.unit
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.bold: true
                text: (updateAsk.major ? qsTr("Move %1 from %2 to %3?") : qsTr("Update %1 from %2 to %3?"))
                          .arg(updateAsk.changes.name || "")
                          .arg(updateAsk.changes.from || "").arg(updateAsk.changes.to || "")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: [updateAsk.line(qsTr("New inputs: %1"), updateAsk.changes.addedInputs),
                       updateAsk.line(qsTr("Inputs that go: %1"), updateAsk.changes.removedInputs),
                       updateAsk.line(qsTr("New outputs: %1"), updateAsk.changes.addedOutputs),
                       updateAsk.line(qsTr("Outputs that go: %1"), updateAsk.changes.removedOutputs)]
                          .filter(l => l !== "").join("\n")
                      || qsTr("The inputs and outputs stay the same.")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: root.look.label
                text: updateAsk.major
                      ? qsTr("The node keeps its values and links, and runs on the new version. Inputs and outputs that go are flagged where they no longer fit; undo moves it back.")
                      : qsTr("Every instance of the group in this workflow moves to the new version; values and links of inputs and outputs that go are dropped.")
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                ThemedButton { text: qsTr("Cancel"); onClicked: updateAsk.close() }
                ThemedButton {
                    objectName: "groupUpdateOk"
                    text: updateAsk.major ? qsTr("Move") : qsTr("Update")
                    highlighted: true
                    onClicked: {
                        updateAsk.close()
                        if (updateAsk.major)
                            root.model.moveToNewerMajor(updateAsk.node)
                        else
                            root.model.updateGroupInstance(updateAsk.node)
                    }
                }
            }
        }
    }

    // A group of the library was renamed in this workflow: rename it there too (#74)? Its folder
    // and id follow; other workflows keep their copy and lose their link to it.
    Question {
        id: libraryAsk
        objectName: "libraryRenameAsk"
        property string libraryId: ""
        property string name: ""
        ColumnLayout {
            anchors.fill: parent
            spacing: 4 * root.look.unit
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("%1 is in the library. Rename it there too, as \u201c%2\u201d?").arg(libraryAsk.libraryId, libraryAsk.name)
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                ThemedButton { text: qsTr("Only here"); onClicked: libraryAsk.close() }
                ThemedButton {
                    objectName: "libraryRenameYes"
                    text: qsTr("Rename in the library")
                    highlighted: true
                    onClicked: {
                        libraryAsk.close()
                        root.model.renameLibraryGroup(libraryAsk.libraryId, libraryAsk.name)
                    }
                }
            }
        }
    }

    // Rename a port on a group's face (#72): its field filled with the label shown now, selected;
    // empty goes back to the inner port's. (Node and Frame titles are edited in place, #74.)
    Question {
        id: renameAsk
        objectName: "renameAsk"
        property string target: ""
        property string port: ""
        property bool output: false
        property string prompt: ""
        function ask(target, port, output, prompt, current) {
            renameAsk.target = target
            renameAsk.port = port
            renameAsk.output = output
            renameAsk.prompt = prompt
            renameField.text = current
            renameAsk.open()
        }
        onOpened: {
            renameField.forceActiveFocus()
            renameField.selectAll()
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 3 * root.look.unit
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label { text: renameAsk.prompt }
            ThemedTextField {
                id: renameField
                objectName: "renameField"
                Layout.fillWidth: true
                onAccepted: renameOk.clicked()
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                ThemedButton { text: qsTr("Cancel"); onClicked: renameAsk.close() }
                ThemedButton {
                    id: renameOk
                    objectName: "renameOk"
                    text: qsTr("Rename")
                    highlighted: true
                    onClicked: {
                        renameAsk.close()
                        root.model.renamePort(renameAsk.target, renameAsk.port, renameAsk.output, renameField.text)
                    }
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
            spacing: 3 * root.look.unit
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Group name") }
                ThemedTextField {
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
                id: faceScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                ScrollBar.vertical: FugueScrollBar { look: root.look }
                ColumnLayout {
                    width: faceScroll.availableWidth - (faceScroll.ScrollBar.vertical as FugueScrollBar).inset
                    spacing: 2 * root.look.unit

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
                                              min: modelData.min, max: modelData.max, type: modelData.type,
                                              default: modelData.default === undefined ? "" : String(modelData.default),
                                              choices: modelData.choices}
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
                                    ThemedTextField {
                                        objectName: "exposedName" + inputRow.index
                                        Layout.preferredWidth: 35 * root.look.unit
                                        text: inputRow.modelData.name
                                        placeholderText: qsTr("name")
                                        onEditingFinished: if (text !== inputRow.modelData.name) inputRow.change("name", text)
                                    }
                                    ThemedTextField {
                                        Layout.fillWidth: true
                                        text: inputRow.modelData.label
                                        placeholderText: qsTr("Label")
                                        onEditingFinished: if (text !== inputRow.modelData.label) inputRow.change("label", text)
                                    }
                                    ThemedTextField {
                                        objectName: "exposedSection" + inputRow.index
                                        Layout.preferredWidth: 30 * root.look.unit
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
                                    ThemedTextField {
                                        Layout.preferredWidth: 22 * root.look.unit
                                        text: inputRow.modelData.min === undefined ? "" : String(inputRow.modelData.min)
                                        placeholderText: qsTr("min")
                                        validator: DoubleValidator {}
                                        onEditingFinished: inputRow.change("min", text)
                                    }
                                    ThemedTextField {
                                        Layout.preferredWidth: 22 * root.look.unit
                                        text: inputRow.modelData.max === undefined ? "" : String(inputRow.modelData.max)
                                        placeholderText: qsTr("max")
                                        validator: DoubleValidator {}
                                        onEditingFinished: inputRow.change("max", text)
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: qsTr("Default") }
                                    ThemedTextField {
                                        objectName: "exposedDefault" + inputRow.index
                                        Layout.preferredWidth: 30 * root.look.unit
                                        text: inputRow.modelData.default === undefined ? "" : String(inputRow.modelData.default)
                                        placeholderText: qsTr("the inner value")
                                        onEditingFinished: inputRow.change("default", text)
                                    }
                                    Label {
                                        visible: choicesField.visible
                                        text: qsTr("Choices")
                                    }
                                    ThemedTextField {
                                        id: choicesField
                                        visible: inputRow.modelData.choices !== "" || inputRow.modelData.innerChoices !== ""
                                        Layout.fillWidth: true
                                        text: inputRow.modelData.choices
                                        placeholderText: inputRow.modelData.innerChoices
                                        onEditingFinished: if (text !== inputRow.modelData.choices) inputRow.change("choices", text)
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
                                    GlyphButton {
                                        glyph: "chevron-up"
                                        enabled: inputRow.index > 0
                                        label: qsTr("Move up")
                                        onClicked: root.model.moveExposed(false, inputRow.modelData.name, inputRow.index - 1)
                                    }
                                    GlyphButton {
                                        glyph: "chevron-down"
                                        enabled: inputRow.index < (editor.face.inputs || []).length - 1
                                        label: qsTr("Move down")
                                        onClicked: root.model.moveExposed(false, inputRow.modelData.name, inputRow.index + 1)
                                    }
                                    GlyphButton {
                                        glyph: "x"
                                        label: qsTr("Stop exposing")
                                        onClicked: root.model.removeExposedInput(inputRow.modelData.name)
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.topMargin: 2 * root.look.unit
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
                            ThemedTextField {
                                Layout.preferredWidth: 35 * root.look.unit
                                text: outputRow.modelData.name
                                onEditingFinished: if (text !== outputRow.modelData.name)
                                                       root.model.renameExposedOutput(outputRow.modelData.name, text)
                            }
                            Label {
                                Layout.fillWidth: true
                                color: root.look.label
                                text: qsTr("from %1 · %2").arg(outputRow.modelData.from).arg(outputRow.modelData.type)
                            }
                            GlyphButton {
                                glyph: "chevron-up"
                                enabled: outputRow.index > 0
                                label: qsTr("Move up")
                                onClicked: root.model.moveExposed(true, outputRow.modelData.name, outputRow.index - 1)
                            }
                            GlyphButton {
                                glyph: "chevron-down"
                                enabled: outputRow.index < (editor.face.outputs || []).length - 1
                                label: qsTr("Move down")
                                onClicked: root.model.moveExposed(true, outputRow.modelData.name, outputRow.index + 1)
                            }
                            GlyphButton {
                                glyph: "x"
                                label: qsTr("Stop exposing")
                                onClicked: root.model.unexposeOutput(outputRow.modelData.name)
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                ThemedButton {
                    objectName: "groupInterfaceClose"
                    text: qsTr("Done")
                    highlighted: true
                    onClicked: editor.close()
                }
            }
        }
    }
}
