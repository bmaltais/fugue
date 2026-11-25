pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../kit"

// Make an app / Edit app (#393, epic #397, spec 12.19): the builder over the workflow on the canvas. Left, what the app is: its title,
// description and the view it opens in; the inputs of its form (a label, Advanced, a range and a default, the inner inputs each one
// drives, the order) and the outputs (the Save and Preview nodes whose results it shows). Right, a live preview of the App view's
// form made from the same list, by the same controls. Inputs and outputs are added by clicking them on the canvas ("Pick on the
// canvas": this dialog steps aside, the canvas outlines what a click would take, Escape or Done brings the dialog back) or by
// the lists under each section, so a keyboard alone does everything. Nothing touches the workflow until Apply, which writes the
// block as one undo step ("Make app", "Edit app"); Cancel, Escape and closing throw the draft away.
// Keys: Tab goes through the fields in reading order; Alt+Up and Alt+Down in a row move it; Enter in a field applies; Escape cancels.
// The work is `App.canvasModel.appBuilder` (apps/app_builder.h).
Item {
    id: root
    readonly property CanvasLook look: App.theme.look
    readonly property CanvasModel model: App.canvasModel
    readonly property AppBuilder builder: model.appBuilder
    property alias dialog: dialog
    // The dialog stepped aside for click-to-expose: it comes back when picking ends.
    property bool pickingAside: false
    // Picking took the window from the App view to the graph: it goes back after.
    property bool leftAppView: false

    Connections {
        target: root.builder
        function onOpenRequested() {
            const tabs = App.workflows.tabs
            const tab = tabs[App.workflows.current]
            if (root.builder.begin(tab && tab.title ? tab.title : ""))
                dialog.open()
            else
                App.workflows.notify(root.builder.notice)
        }
        function onStateChanged() {
            if (root.builder.open && !root.builder.picking && root.pickingAside) {
                root.pickingAside = false
                if (root.leftAppView && App.appView)
                    App.appView.showApp()
                root.leftAppView = false
                dialog.open()
            } else if (!root.builder.open && dialog.visible) {
                root.pickingAside = false
                dialog.close()
            }
        }
    }

    // The way out of click-to-expose, over the canvas: made only while picking (the window keeps few items, #5).
    Component {
        id: pickBannerComponent
        Rectangle {
            objectName: "appPickBanner"
            anchors.horizontalCenter: parent.horizontalCenter
            y: 12 * root.look.unit
            width: Math.min(parent.width - 8 * root.look.unit, bannerRow.implicitWidth + 6 * root.look.unit)
            height: bannerRow.implicitHeight + 3 * root.look.unit
            radius: Math.min(root.look.popupCorner, height / 2)
            color: root.look.popup
            border.width: 1
            border.color: root.look.hairline
            RowLayout {
                id: bannerRow
                anchors.centerIn: parent
                width: parent.width - 4 * root.look.unit
                spacing: 3 * root.look.unit
                Text {
                    Layout.fillWidth: true
                    text: root.builder.notice
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.look.text
                    font.pixelSize: root.look.textSize
                }
                ViewButton {
                    objectName: "appPickDone"
                    look: root.look
                    primary: true
                    text: qsTr("Done")
                    tip: qsTr("Back to the app builder (Escape)")
                    onTapped: root.builder.finishPicking()
                }
            }
        }
    }
    property Item pickBanner: null
    Connections {
        target: root.builder
        function onStateChanged() {
            if (root.builder.picking && !root.pickBanner)
                root.pickBanner = pickBannerComponent.createObject(root) as Item
            else if (!root.builder.picking && root.pickBanner) {
                root.pickBanner.destroy()
                root.pickBanner = null
            }
        }
    }

    Question {
        id: dialog
        objectName: "appBuilderDialog"
        heading: root.builder.editing ? qsTr("Edit app") : qsTr("Make an app")
        subheading: qsTr("Pick what the form asks for and what it shows.")
        tone: "sea"
        width: Math.min((Overlay.overlay ? Overlay.overlay.width : 1000) - 12 * look.unit, 1000)
        height: Math.min((Overlay.overlay ? Overlay.overlay.height : 700) - 12 * look.unit, 720)
        onOpened: titleField.forceFocus()
        onClosed: if (!root.pickingAside && root.builder.open) root.builder.cancel()

        function apply(openAppView) {
            if (root.builder.apply(openAppView))
                close()
        }

        // The kit's recessed field (AppField, the App form's own); Enter applies.
        component Field: AppField {
            property string hint
            look: root.look
            Layout.fillWidth: true
            placeholder: hint
            onAccepted: dialog.apply(false)
        }

        // One input of the form.
        component InputRow: Item {
            id: row
            required property var modelData
            required property int index
            readonly property string key: modelData.key
            width: parent ? parent.width : implicitWidth
            implicitHeight: column.implicitHeight + 3 * root.look.unit
            height: implicitHeight
            Keys.onPressed: event => {
                if (event.modifiers & Qt.AltModifier && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                    root.builder.moveInput(row.key, row.index + (event.key === Qt.Key_Up ? -1 : 1))
                    event.accepted = true
                }
            }
            KitSeparator { look: root.look; visible: row.index > 0 }
            ColumnLayout {
                id: column
                x: 3 * root.look.unit
                y: 1.5 * root.look.unit
                width: parent.width - 6 * root.look.unit
                spacing: root.look.unit
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2 * root.look.unit
                    Text {
                        text: row.modelData.kind
                        color: root.look.label
                        font.pixelSize: root.look.labelSize
                        Layout.preferredWidth: 11 * root.look.unit
                    }
                    Field {
                        objectName: "builderLabel-" + row.index
                        text: row.modelData.label
                        hint: row.modelData.shown
                        onEditingFinished: if (text !== row.modelData.label) root.builder.editInput(row.key, {label: text})
                    }
                    Text {
                        text: qsTr("Advanced")
                        color: root.look.label
                        font.pixelSize: root.look.labelSize
                    }
                    KitToggle {
                        objectName: "builderAdvanced-" + row.index
                        look: root.look
                        text: qsTr("Advanced")
                        checked: row.modelData.advanced
                        onToggled: value => root.builder.editInput(row.key, {advanced: value})
                    }
                    ViewButton {
                        objectName: "builderUp-" + row.index
                        look: root.look
                        flat: true
                        compact: true
                        glyph: "chevron-up"
                        tip: qsTr("Move up")
                        enabled: !row.modelData.first
                        onTapped: root.builder.moveInput(row.key, row.index - 1)
                    }
                    ViewButton {
                        objectName: "builderDown-" + row.index
                        look: root.look
                        flat: true
                        compact: true
                        glyph: "chevron-down"
                        tip: qsTr("Move down")
                        enabled: !row.modelData.last
                        onTapped: root.builder.moveInput(row.key, row.index + 1)
                    }
                    ViewButton {
                        objectName: "builderRemove-" + row.index
                        look: root.look
                        flat: true
                        compact: true
                        glyph: "x"
                        tip: qsTr("Stop asking for this")
                        onTapped: root.builder.removeInput(row.key)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2 * root.look.unit
                    visible: row.modelData.ranged
                    Text {
                        text: qsTr("Range")
                        color: root.look.label
                        font.pixelSize: root.look.labelSize
                    }
                    AppField {
                        objectName: "builderMin-" + row.index
                        look: root.look
                        Layout.preferredWidth: 20 * root.look.unit
                        text: row.modelData.min
                        placeholder: (row.modelData.innerMin !== "" ? row.modelData.innerMin : qsTr("min"))
                        validator: DoubleValidator {}
                        onEditingFinished: if (text !== row.modelData.min) root.builder.editInput(row.key, {min: text})
                        onAccepted: dialog.apply(false)
                    }
                    AppField {
                        objectName: "builderMax-" + row.index
                        look: root.look
                        Layout.preferredWidth: 20 * root.look.unit
                        text: row.modelData.max
                        placeholder: (row.modelData.innerMax !== "" ? row.modelData.innerMax : qsTr("max"))
                        validator: DoubleValidator {}
                        onEditingFinished: if (text !== row.modelData.max) root.builder.editInput(row.key, {max: text})
                        onAccepted: dialog.apply(false)
                    }
                    Item { Layout.fillWidth: true }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2 * root.look.unit
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Sets %1").arg(row.modelData.titles.join(", "))
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: root.look.label
                        font.pixelSize: root.look.labelSize
                    }
                    AppChoice {
                        id: alsoDrive
                        objectName: "builderAlso-" + row.index
                        look: root.look
                        readonly property var candidates: dialog.visible ? root.builder.driveCandidates(row.key) : []
                        visible: candidates.length > 0
                        label: qsTr("Also set another input")
                        value: ""
                        options: [{value: "", text: qsTr("Also set…")}].concat(candidates.map((c, i) => ({value: String(i), text: c.title + " · " + c.label})))
                        onPicked: v => {
                            if (v !== "") {
                                const c = candidates[Number(v)]
                                root.builder.alsoDrive(row.key, c.node, c.input)
                            }
                        }
                    }
                }
            }
        }

        // One output of the app.
        component OutputRow: Item {
            id: row
            required property var modelData
            required property int index
            width: parent ? parent.width : implicitWidth
            implicitHeight: line.implicitHeight + 3 * root.look.unit
            height: implicitHeight
            Keys.onPressed: event => {
                if (event.modifiers & Qt.AltModifier && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                    root.builder.moveOutput(row.modelData.node, row.index + (event.key === Qt.Key_Up ? -1 : 1))
                    event.accepted = true
                }
            }
            KitSeparator { look: root.look; visible: row.index > 0 }
            RowLayout {
                id: line
                x: 3 * root.look.unit
                y: 1.5 * root.look.unit
                width: parent.width - 6 * root.look.unit
                spacing: 2 * root.look.unit
                Text {
                    Layout.preferredWidth: 20 * root.look.unit
                    text: row.modelData.title
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: row.modelData.sink ? root.look.text : root.look.error
                    font.pixelSize: root.look.textSize
                }
                Field {
                    objectName: "builderOutputLabel-" + row.index
                    text: row.modelData.label
                    hint: row.modelData.title
                    onEditingFinished: if (text !== row.modelData.label) root.builder.setOutputLabel(row.modelData.node, text)
                }
                ViewButton {
                    objectName: "builderOutputUp-" + row.index
                    look: root.look
                    flat: true
                    compact: true
                    glyph: "chevron-up"
                    tip: qsTr("Move up")
                    enabled: !row.modelData.first
                    onTapped: root.builder.moveOutput(row.modelData.node, row.index - 1)
                }
                ViewButton {
                    objectName: "builderOutputDown-" + row.index
                    look: root.look
                    flat: true
                    compact: true
                    glyph: "chevron-down"
                    tip: qsTr("Move down")
                    enabled: !row.modelData.last
                    onTapped: root.builder.moveOutput(row.modelData.node, row.index + 1)
                }
                ViewButton {
                    objectName: "builderOutputRemove-" + row.index
                    look: root.look
                    flat: true
                    compact: true
                    glyph: "x"
                    tip: qsTr("Stop showing this")
                    onTapped: root.builder.removeOutput(row.modelData.node)
                }
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 3 * root.look.unit
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 4 * root.look.unit

                // What the app is, its inputs and outputs.
                ScrollView {
                    id: leftScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 3
                    contentWidth: availableWidth
                    clip: true
                    ScrollBar.vertical: FugueScrollBar { look: root.look }
                    Column {
                        width: leftScroll.availableWidth - (leftScroll.ScrollBar.vertical as FugueScrollBar).inset
                        KitGroup {
                            look: root.look
                            KitRow {
                                look: root.look
                                label: qsTr("Title")
                                Field {
                                    id: titleField
                                    objectName: "builderTitle"
                                    width: 44 * root.look.unit
                                    text: root.builder.title
                                    onTextEdited: root.builder.title = text
                                }
                            }
                            KitRow {
                                look: root.look
                                label: qsTr("Description")
                                hint: qsTr("One line under the title")
                                Field {
                                    objectName: "builderDescription"
                                    width: 44 * root.look.unit
                                    text: root.builder.description
                                    onTextEdited: root.builder.description = text
                                }
                            }
                            KitRow {
                                look: root.look
                                label: qsTr("Opens in")
                                KitSegmented {
                                    objectName: "builderView"
                                    look: root.look
                                    text: qsTr("Opens in")
                                    options: [qsTr("App"), qsTr("Graph")]
                                    optionNames: ["builderViewApp", "builderViewGraph"]
                                    currentIndex: root.builder.defaultView === "graph" ? 1 : 0
                                    onMoved: index => root.builder.defaultView = index === 1 ? "graph" : "app"
                                }
                            }
                        }

                        KitSection { look: root.look; text: qsTr("Inputs") }
                        RowLayout {
                            width: parent.width
                            spacing: 2 * root.look.unit
                            ViewButton {
                                objectName: "builderPick"
                                look: root.look
                                text: qsTr("Pick on the canvas")
                                tip: qsTr("Click inputs and Save nodes in the graph to add them")
                                onTapped: {
                                    root.pickingAside = true
                                    if (App.appView && App.appView.shown) {
                                        root.leftAppView = true
                                        App.appView.openInGraph()
                                    }
                                    dialog.close()
                                    root.builder.startPicking()
                                }
                            }
                            AppChoice {
                                id: addInput
                                objectName: "builderAddInput"
                                look: root.look
                                Layout.fillWidth: true
                                readonly property var choices: dialog.visible ? root.builder.inputChoices().filter(c => !c.exposed) : []
                                label: qsTr("Add an input")
                                value: ""
                                options: [{value: "", text: qsTr("Add an input…")}].concat(choices.map((c, i) => ({value: String(i), text: c.title + " · " + c.label})))
                                onPicked: v => {
                                    if (v !== "")
                                        root.builder.exposeInput(choices[Number(v)].node, choices[Number(v)].input)
                                }
                            }
                        }
                        Item { width: 1; height: root.look.unit }
                        Text {
                            visible: root.builder.inputs.length === 0
                            width: parent.width
                            leftPadding: 2 * root.look.unit
                            bottomPadding: root.look.unit
                            wrapMode: Text.Wrap
                            color: root.look.label
                            font.pixelSize: root.look.labelSize
                            text: qsTr("Nothing yet. Pick on the canvas, or add one from the list.")
                        }
                        KitGroup {
                            look: root.look
                            visible: root.builder.inputs.length > 0
                            Repeater {
                                model: root.builder.inputs
                                delegate: InputRow {}
                            }
                        }
                        KitSection { look: root.look; text: qsTr("Outputs") }
                        AppChoice {
                            id: addOutput
                            objectName: "builderAddOutput"
                            look: root.look
                            width: parent.width
                            readonly property var choices: dialog.visible ? root.builder.outputChoices().filter(c => !c.added) : []
                            label: qsTr("Add an output")
                            value: ""
                            options: [{value: "", text: qsTr("Add an output…")}].concat(choices.map((c, i) => ({value: String(i), text: c.title})))
                            onPicked: v => {
                                if (v !== "")
                                    root.builder.addOutput(choices[Number(v)].node)
                            }
                        }
                        Item { width: 1; height: root.look.unit }
                        KitGroup {
                            look: root.look
                            visible: root.builder.outputs.length > 0
                            Repeater {
                                model: root.builder.outputs
                                delegate: OutputRow {}
                            }
                        }
                    }
                }

                // The App view's form, as this list makes it.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 2
                    spacing: 0
                    Text {
                        text: qsTr("Preview")
                        color: root.look.text
                        font.pixelSize: root.look.resultSize
                        font.bold: true
                        leftPadding: 2 * root.look.unit
                        bottomPadding: 1.5 * root.look.unit
                    }
                    ScrollView {
                        id: previewScroll
                        objectName: "builderPreview"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        contentWidth: availableWidth
                        clip: true
                        readonly property var fields: root.builder.preview.fields
                        Column {
                            width: previewScroll.availableWidth
                            Text {
                                width: parent.width
                                leftPadding: 2 * root.look.unit
                                bottomPadding: root.look.unit
                                text: root.builder.title
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                                color: root.look.text
                                font.pixelSize: root.look.resultSize
                                font.bold: true
                            }
                            Text {
                                visible: text !== ""
                                width: parent.width
                                leftPadding: 2 * root.look.unit
                                rightPadding: 2 * root.look.unit
                                bottomPadding: 2 * root.look.unit
                                text: root.builder.description
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                                color: root.look.label
                                font.pixelSize: root.look.labelSize
                            }
                            KitGroup {
                                look: root.look
                                enabled: false // a picture of the form: nothing here is edited
                                visible: previewScroll.fields.length > 0
                                Repeater {
                                    model: previewScroll.fields
                                    delegate: AppInput {
                                        required property var modelData
                                        appView: root.builder.preview
                                        field: modelData
                                    }
                                }
                            }
                            Text {
                                visible: previewScroll.fields.length === 0
                                width: parent.width
                                leftPadding: 2 * root.look.unit
                                wrapMode: Text.Wrap
                                color: root.look.label
                                font.pixelSize: root.look.labelSize
                                text: qsTr("The form is empty.")
                            }
                            Text {
                                visible: root.builder.outputs.length > 0
                                width: parent.width
                                topPadding: 2 * root.look.unit
                                leftPadding: 2 * root.look.unit
                                wrapMode: Text.Wrap
                                color: root.look.label
                                font.pixelSize: root.look.labelSize
                                text: qsTr("Shows: %1").arg(root.builder.outputs.map(o => o.label !== "" ? o.label : o.title).join(", "))
                            }
                        }
                    }
                }
            }

            // Why Apply is off, and what the last pick said.
            Text {
                objectName: "builderProblems"
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.Wrap
                color: root.look.error
                font.pixelSize: root.look.labelSize
                text: root.builder.problems.join("  ")
            }
            Text {
                objectName: "builderNotice"
                Layout.fillWidth: true
                visible: text !== "" && !root.builder.picking
                wrapMode: Text.Wrap
                color: root.look.label
                font.pixelSize: root.look.labelSize
                text: root.builder.notice
            }

            QuestionAnswers {
                leadingSpacer: !root.builder.editing
                ThemedButton {
                    objectName: "builderRemoveApp"
                    visible: root.builder.editing
                    text: qsTr("Remove app")
                    flat: true
                    danger: true
                    onClicked: {
                        if (root.builder.removeApp())
                            dialog.close()
                    }
                }
                Item {
                    visible: root.builder.editing
                    Layout.fillWidth: true
                }
                ThemedButton {
                    objectName: "builderCancel"
                    text: qsTr("Cancel")
                    onClicked: dialog.close()
                }
                ThemedButton {
                    objectName: "builderApply"
                    text: qsTr("Apply")
                    enabled: root.builder.problems.length === 0
                    onClicked: dialog.apply(false)
                }
                ThemedButton {
                    objectName: "builderApplyOpen"
                    text: qsTr("Apply and open App view")
                    highlighted: true
                    enabled: root.builder.problems.length === 0
                    onClicked: dialog.apply(true)
                }
            }
        }
    }
}
