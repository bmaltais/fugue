pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../menu"
import "../canvas"
import "../apps"
import "../kit"

// The model folder setup (spec 8.9, #619; docs/ui-ergonomics.md): one screen for each step, on the Drift kit. First a consent screen
// that says why and lists every place Fugue would look with a switch each (Search, Add a folder…, Skip); only after Search the
// results: what was found with a switch each, the folder new downloads go in, and Use N folders. Everything is
// App.settings.modelSetup (C++, settings/model_setup.h); this only shows it. Nothing is searched until Search, a place left off is
// never touched, and nothing is written until the yes.
//
// A Moss KitPanel (models and downloads, like Get ready), 680 px at most, modal. Keys: opening focuses Skip (on the consent screen and while it looks), then
// the primary; Tab walks the switches in reading order, then Add a folder…, the download choice, Skip and the primary; Up and Down
// move between the switches, Space flips one; Enter anywhere runs the primary (a switch takes no Enter); Escape is Skip (a rerun:
// close with nothing written), and the focus goes back to what had it.
Popup {
    id: sheet
    objectName: "modelSetupSheet"
    property var setup: App.settings.modelSetup // not readonly: the picture tests give it a made-up setup
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    readonly property bool rightToLeft: Application.layoutDirection === Qt.RightToLeft
    readonly property bool consent: setup.phase === "consent"
    readonly property int firstRows: 8
    property bool showAll: false
    visible: setup.open
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min((Overlay.overlay ? Overlay.overlay.width : 680) - 12 * u, 680)
    exit: null
    Overlay.modal: Rectangle { color: App.theme.look.scrim }
    background: Item {
        Shadow { target: box; level: 3; look: sheet.look }
        Item { id: box; anchors.fill: parent }
    }

    // What had the focus before: the sheet gives it back when it goes.
    property Item opener: null
    onAboutToShow: { opener = parent ? parent.Window.activeFocusItem : null; showAll = false }
    onOpened: focusDefault()
    onClosed: if (opener) opener.forceActiveFocus()
    // Skip on the consent screen and while it looks (the safe answer: Enter never starts a search by habit), the primary once there is
    // something to say yes to.
    readonly property Item defaultControl: consent || setup.looking ? skip : primary
    function focusDefault() { defaultControl.forceActiveFocus() }
    // When looking ends the focus follows to the primary, unless the person has moved it.
    onDefaultControlChanged: if (opened) Qt.callLater(refocus)
    function refocus() {
        if (!opened)
            return
        let item = parent ? parent.Window.activeFocusItem : null
        const lost = item === null || item === card || item === skip || item === primary
        while (item && item !== card)
            item = item.parent
        if (lost || !item)
            focusDefault()
    }

    FilePicker {
        id: folderPicker
        objectName: "modelSetupFolderDialog"
        title: qsTr("Pick a folder of models")
        fileMode: PortalFileDialog.Directory
        onAccepted: sheet.add(selectedFolder)
    }
    function add(folder) {
        const why = setup.addFolder(folder)
        problem = why
    }
    property string problem // what went wrong with the folder just picked

    // A row of the results: it knows its switch, so Up and Down can find the next one.
    component SetupRow: KitRow {
        property Item toggle: null
        readonly property bool hasToggle: toggle !== null && toggle.visible
    }

    contentItem: KitPanel {
        id: card
        objectName: "modelSetupCard"
        look: sheet.look
        tone: "moss"
        clip: true
        implicitHeight: body.implicitHeight + 2
        focus: true
        // Tab walks the sheet's controls: it is also the window's "Add a node by search" key.
        Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab
        // The keys nothing under the focus took: Enter is the primary, Escape is Skip.
        Keys.onEscapePressed: event => { sheet.setup.skip(); event.accepted = true }
        Keys.onReturnPressed: event => { sheet.run(); event.accepted = true }
        Keys.onEnterPressed: event => { sheet.run(); event.accepted = true }

        ColumnLayout {
            id: body
            anchors { fill: parent; margins: 1 }
            spacing: 0
            LayoutMirroring.enabled: sheet.rightToLeft
            LayoutMirroring.childrenInherit: true

            KitHero {
                Layout.fillWidth: true
                look: sheet.look
                tone: card.tone
                title: qsTr("Your model folders")
                subtitle: qsTr("Fugue can use the models you already have. Nothing is moved or copied.")
            }

            Flickable {
                id: list
                objectName: "modelSetupList"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(rows.implicitHeight + 2 * sheet.u, 0.55 * (Overlay.overlay ? Overlay.overlay.height : 800))
                clip: true
                contentWidth: width
                contentHeight: rows.implicitHeight + 2 * sheet.u
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: FugueScrollBar { look: sheet.look }

                Column {
                    id: rows
                    x: 4 * sheet.u
                    width: list.width - 8 * sheet.u - (list.ScrollBar.vertical as FugueScrollBar).breadth

                    // --- 1. the consent ---
                    Text {
                        objectName: "modelSetupWhy"
                        visible: sheet.consent
                        width: parent.width
                        topPadding: 3 * sheet.u
                        wrapMode: Text.Wrap
                        color: sheet.look.text
                        font.pixelSize: sheet.look.textSize
                        text: qsTr("Fugue can look for model folders you already have (ComfyUI, Forge, A1111, InvokeAI, SwarmUI, Stability Matrix) so templates use your models instead of downloading them again. It only reads folder names and file sizes; nothing is opened, changed or sent anywhere.")
                    }
                    KitSection {
                        look: sheet.look
                        visible: sheet.consent
                        text: qsTr("Places Fugue would look")
                    }
                    KitGroup {
                        look: sheet.look
                        visible: sheet.consent
                        Repeater {
                            // The delegates stay when a switch flips (the focus stays on it): the model is the count, each row reads its own.
                            model: sheet.consent ? sheet.setup.locations.length : 0
                            KitRow {
                                id: place
                                required property int index
                                readonly property var modelData: sheet.setup.locations[index] ?? ({id: "", display: "", hint: "", checked: false})
                                objectName: "modelSetupPlace-" + modelData.id
                                look: sheet.look
                                label: modelData.display
                                hint: modelData.hint
                                target: placeToggle
                                KitToggle {
                                    id: placeToggle
                                    objectName: "modelSetupPlaceToggle-" + place.modelData.id
                                    look: sheet.look
                                    text: place.modelData.display
                                    checked: place.modelData.checked
                                    onToggled: value => sheet.setup.setLocationChecked(place.modelData.id, value)
                                }
                            }
                        }
                    }
                    Text {
                        visible: sheet.consent
                        width: parent.width
                        topPadding: 2 * sheet.u
                        wrapMode: Text.Wrap
                        color: sheet.look.label
                        font.pixelSize: sheet.look.labelSize
                        text: qsTr("Fugue does not look in a place you switch off.")
                    }

                    // --- 2. the results ---
                    KitSection {
                        look: sheet.look
                        visible: !sheet.consent
                        text: qsTr("Found on this computer")
                        topPadding: 3 * sheet.u
                    }
                    Text {
                        objectName: "modelSetupNothing"
                        visible: sheet.setup.nothing
                        width: parent.width
                        wrapMode: Text.Wrap
                        color: sheet.look.text
                        font.pixelSize: sheet.look.textSize
                        text: qsTr("Fugue found no model folders. If you have models, add their folder; if not, Fugue downloads what a template needs when you open it.")
                    }
                    KitGroup {
                        look: sheet.look
                        visible: !sheet.consent && !sheet.setup.nothing
                        // While it looks: a quiet moving bar and the line that says so.
                        KitRow {
                            objectName: "modelSetupLooking"
                            look: sheet.look
                            visible: sheet.setup.looking
                            label: qsTr("Looking in your home folder and drives…")
                            Item {
                                implicitWidth: 16 * sheet.u
                                implicitHeight: 1.5 * sheet.u
                                Rectangle { anchors.fill: parent; radius: height / 2; color: sheet.look.kitTrack }
                                Rectangle {
                                    id: sweep
                                    height: parent.height
                                    width: parent.width / 3
                                    radius: height / 2
                                    color: sheet.look.accent
                                    SequentialAnimation on x {
                                        running: sheet.setup.looking && sheet.visible
                                        loops: Animation.Infinite
                                        NumberAnimation { from: 0; to: sweep.parent.width * 2 / 3; duration: 900; easing.type: Easing.InOutQuad }
                                        NumberAnimation { from: sweep.parent.width * 2 / 3; to: 0; duration: 900; easing.type: Easing.InOutQuad }
                                    }
                                }
                            }
                        }
                        Repeater {
                            id: found
                            model: sheet.consent ? 0 : sheet.setup.rows.length
                            SetupRow {
                                id: folderRow
                                required property int index
                                readonly property var modelData: sheet.setup.rows[index] ?? ({path: "", display: "", title: "", hint2: "", tooltip: "", on: false,
                                                                                                inUse: false, inherited: false, needsKind: false, kind: "", note: ""})
                                objectName: "modelSetupRow-" + index
                                look: sheet.look
                                visible: sheet.showAll || index < sheet.firstRows
                                label: modelData.title
                                hintLines: modelData.note !== "" ? 4 : 2
                                hint: pathCut.elidedText + "\n" + modelData.hint2 + (modelData.note !== "" ? "\n" + modelData.note : "")
                                target: modelData.inherited ? null : rowToggle
                                toggle: modelData.inherited ? null : rowToggle
                                // The path is cut in the middle to the width the hint has.
                                TextMetrics {
                                    id: pathCut
                                    font.pixelSize: sheet.look.labelSize
                                    elide: Text.ElideMiddle
                                    elideWidth: Math.max(10 * sheet.u, sheet.width - 40 * sheet.u)
                                    text: folderRow.modelData.display
                                }
                                HoverHandler { id: rowHover }
                                ToolTip.visible: rowHover.hovered && modelData.tooltip !== ""
                                ToolTip.delay: sheet.look.tooltipDelay
                                ToolTip.text: modelData.tooltip
                                AppChoice {
                                    objectName: "modelSetupKind-" + folderRow.index
                                    visible: folderRow.modelData.needsKind
                                    look: sheet.look
                                    label: qsTr("Choose what it holds")
                                    options: sheet.setup.kinds()
                                    value: folderRow.modelData.kind
                                    text: value === "" ? qsTr("Choose what it holds") : shownText
                                    ink: value === "" ? sheet.look.label : sheet.look.text
                                    onPicked: v => sheet.setup.setRowKind(folderRow.modelData.path, v)
                                }
                                KitBadge {
                                    visible: folderRow.modelData.inUse && !folderRow.modelData.inherited
                                    look: sheet.look
                                    text: qsTr("In use")
                                }
                                KitBadge {
                                    visible: folderRow.modelData.inherited
                                    look: sheet.look
                                    text: qsTr("Inherited from Default")
                                }
                                KitToggle {
                                    id: rowToggle
                                    objectName: "modelSetupToggle-" + folderRow.index
                                    visible: folderRow.hasToggle
                                    look: sheet.look
                                    text: folderRow.modelData.display
                                    checked: folderRow.modelData.on
                                    onToggled: value => sheet.setup.setRowOn(folderRow.modelData.path, value)
                                    Keys.onUpPressed: event => { sheet.move(folderRow.index, -1); event.accepted = true }
                                    Keys.onDownPressed: event => { sheet.move(folderRow.index, 1); event.accepted = true }
                                }
                            }
                        }
                        KitRow {
                            objectName: "modelSetupMore"
                            look: sheet.look
                            visible: !sheet.showAll && sheet.setup.rows.length > sheet.firstRows
                            label: qsTr("Show %n more", "", sheet.setup.rows.length - sheet.firstRows)
                            KitChip {
                                objectName: "modelSetupShowMore"
                                look: sheet.look
                                text: qsTr("Show %n more", "", sheet.setup.rows.length - sheet.firstRows)
                                onTapped: sheet.showAll = true
                            }
                        }
                    }
                    Text {
                        visible: !sheet.consent && !sheet.setup.looking && !sheet.setup.nothing
                        width: parent.width
                        topPadding: 2 * sheet.u
                        wrapMode: Text.Wrap
                        color: sheet.look.label
                        font.pixelSize: sheet.look.labelSize
                        text: qsTr("Fugue looked in the usual places. Add any folder it missed.")
                    }
                    Text {
                        objectName: "modelSetupProblem"
                        visible: sheet.problem !== ""
                        width: parent.width
                        topPadding: sheet.u
                        wrapMode: Text.Wrap
                        color: sheet.look.error
                        font.pixelSize: sheet.look.labelSize
                        text: sheet.problem
                    }
                    Item { width: 1; height: 2 * sheet.u }
                    KitChip {
                        id: addFolder
                        objectName: "modelSetupAdd"
                        look: sheet.look
                        bordered: true
                        text: qsTr("Add a folder…")
                        onTapped: folderPicker.open()
                    }
                    Item { width: 1; height: 2 * sheet.u }

                    KitSection {
                        look: sheet.look
                        visible: !sheet.consent && sheet.setup.choices.length > 1
                        text: qsTr("New downloads")
                    }
                    KitGroup {
                        look: sheet.look
                        visible: !sheet.consent && sheet.setup.choices.length > 1
                        KitRow {
                            id: downloadRow
                            objectName: "modelSetupDownload"
                            look: sheet.look
                            label: qsTr("Download new models to")
                            hintLines: 2
                            hint: sheet.setup.downloadHint + (sheet.setup.downloadWarning !== "" ? "\n" + sheet.setup.downloadWarning : "")
                            AppChoice {
                                id: downloadChoice
                                objectName: "modelSetupDownloadChoice"
                                look: sheet.look
                                label: qsTr("Download new models to")
                                options: sheet.setup.choices.map(c => ({value: c.value, text: c.text}))
                                value: sheet.setup.download
                                // The button says which folder (cut at the end of a long path); the menu says how much room each has.
                                text: sheet.setup.choices.find(c => c.value === sheet.setup.download)?.title ?? ""
                                width: Math.min(implicitWidth, 36 * sheet.u)
                                onPicked: v => sheet.setup.setDownload(v)
                            }
                        }
                    }
                    Item { width: 1; height: 2 * sheet.u }
                }
            }

            // What went wrong in the yes, then the answers: the safe one first, the primary last at the end (D9).
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 6 * sheet.u
                Layout.rightMargin: 6 * sheet.u
                Layout.topMargin: sheet.u
                spacing: sheet.u
                Text {
                    objectName: "modelSetupMessage"
                    visible: sheet.setup.message !== ""
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: sheet.look.error
                    font.pixelSize: sheet.look.textSize
                    text: sheet.setup.message
                }
                Text {
                    visible: !sheet.consent
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: sheet.look.label
                    font.pixelSize: sheet.look.labelSize
                    text: qsTr("You can change this later in Settings > Folders.")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 4 * sheet.u
                spacing: 2 * sheet.u
                Item { Layout.fillWidth: true }
                KitChip {
                    id: skip
                    objectName: "modelSetupSkip"
                    look: sheet.look
                    bordered: true
                    text: qsTr("Skip")
                    onTapped: sheet.setup.skip()
                }
                KitChip {
                    id: primary
                    objectName: "modelSetupPrimary"
                    look: sheet.look
                    primary: true
                    enabled: sheet.setup.primaryEnabled
                    text: sheet.setup.primaryText
                    onTapped: sheet.run()
                }
            }
        }
    }

    function run() {
        if (setup.primaryEnabled)
            setup.apply()
    }
    // Up and Down move between the switches of the rows (those that have one and are shown).
    function move(from, delta) {
        for (let i = from + delta; i >= 0 && i < found.count; i += delta) {
            const row = found.itemAt(i) as SetupRow
            if (row && row.visible && row.hasToggle) {
                row.toggle.forceActiveFocus(Qt.TabFocusReason)
                return
            }
        }
    }
}
