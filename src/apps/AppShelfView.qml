pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue
import "../kit"

// The apps shelf as one kit panel (#463, #470): the Dusk header ("Apps" and what to do, the buttons frosted over it), the search
// field, and the grid of cards. One component for the home page and the File > Apps… dialog, so they cannot differ: the dialog adds
// the header's close button. Everything reads the look parameters, so the Flat preset gives the flat version from the same code.
// The search narrows the shelf live (AppShelf.query); Down, or Enter on the first match, goes to the cards; Up from the first row
// comes back. `focusSearch()` is where the keyboard goes when it opens.
KitPanel {
    id: view
    objectName: "appShelfView"

    property bool asDialog: false // a dialog: the header has a close button
    readonly property real u: look.unit
    signal closeRequested
    signal chosen(string id)

    function focusSearch() { search.focusField() }
    function focusCards() { grid.focusCards() }
    function clearSearch() { search.text = "" }

    tone: "dusk"
    clip: true
    // Tab walks the shelf's controls: it is also the window's "Add a node by search" key, which has no business here.
    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab

    ColumnLayout {
        anchors { fill: parent; margins: 1 }
        spacing: 0

        KitHero {
            id: hero
            objectName: "appsHero"
            Layout.fillWidth: true
            look: view.look
            title: qsTr("Apps")
            subtitle: qsTr("Pick one, give it a picture or a few words, press Run.")
            tone: view.tone
            showClose: view.asDialog
            onClosed: view.closeRequested()
            HeroChip {
                id: browse
                objectName: view.asDialog ? "appsDialogBrowse" : "appsBrowseTemplates"
                KeyNavigation.tab: blank.visible ? blank : search
                KeyNavigation.backtab: grid.cardsItem
                glyph: "layout-template"
                text: qsTr("Browse templates")
                tip: qsTr("All the templates, apps and workflows")
                onTapped: {
                    if (view.asDialog)
                        view.closeRequested()
                    App.commands.trigger("file.templates")
                }
            }
            HeroChip {
                id: blank
                objectName: "appsStartBlank"
                KeyNavigation.tab: search
                KeyNavigation.backtab: browse
                visible: !view.asDialog
                glyph: "workflow"
                text: qsTr("Start blank")
                tip: qsTr("An empty graph, to build a workflow from nothing")
                onTapped: App.appShelf.dismiss()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: view.look.params.panelPad * view.u
            Layout.bottomMargin: 0
            KitSearch {
                id: search
                objectName: "appsSearch"
                Layout.fillWidth: true
                look: view.look
                placeholder: qsTr("Search apps")
                KeyNavigation.tab: grid.cardsItem
                KeyNavigation.backtab: blank.visible ? blank : browse
                onTextChanged: App.appShelf.query = text
                onDown: grid.focusAt(0)
                onAccepted: if (grid.apps.length > 0) view.chosen(grid.apps[0].id)
            }
        }

        AppShelfGrid {
            id: grid
            // Tab goes round the shelf: the header's buttons, the search, the cards. (Set here, in the order the person reads it:
            // the window's focus chain skips the shelf's items when it is home, and Tab is a window shortcut.)
            tabTarget: browse
            backtabTarget: search
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: view.look.params.panelPad * view.u - view.look.params.areaGap * view.u / 2
            onChosen: id => view.chosen(id)
            onSearchRequested: search.focusField()
        }
    }

    // A small frosted button over the header's picture or blend.
    component HeroChip: KitChip {
        look: view.look
        frosted: true
        backdrop: hero.backdropItem
        backdropSource: hero.shownSource
    }
}
