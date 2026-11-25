pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../canvas"
import "../kit"

// One app on the shelf (#388, #463, #470): a kit card (raised, the Corners and Spacing sliders move it) with its picture, which
// moves while the card is in view and the window is focused (the app's WebM, `entry.video`; KitVideo keeps the budget and the
// poster stays under reduced motion), its name, one line of what it does, and one line of whether it is ready: a check and
// "Ready", or what it still needs ("Needs 2.1 GB and 1 pack"). Over the picture, frosted: the bookmark (pins the app to the top)
// and the menu (Duplicate, Rename). The whole card is the one button; choosing opens the app, and an app that is not ready asks
// for what it needs once, on the one Get ready sheet. The Templates window's card, smaller: no details chip and no download
// button, which are for browsing, not for starting (docs/ui-ergonomics.md, "Apps shelf").
//
// `entry` is one entry of AppShelf.apps (apps/app_shelf.h). The owner sets `current` (the grid's focused card), `inView` (the card
// is on screen) and `renaming`, and listens to `chosen`, `bookmarkToggled`, `menuRequested` and `renamed`/`renameCancelled`.
KitCard {
    id: card

    required property var entry // the shelf's entry: {id, title, summary, video, ready, needs, detail, local, hosts, offlineHosts, bookmarked...}
    property bool inView: true
    property bool renaming: false
    signal chosen
    signal bookmarkToggled
    signal menuRequested(Item anchor)
    signal renamed(string title)
    signal renameCancelled

    readonly property real u: look.unit
    readonly property real pad: look.params.rowPadY * u
    // The picture plays only while the card is hovered or has the keyboard's place (calm, and one clip at a time:
    // the owner's "calm" and "not taxing" rules); KitVideo adds the window's focus and reduced motion.
    readonly property bool playing: hovered || current
    // The two frosted buttons show when the card is hovered, has the keyboard's place, or (the bookmark) holds a pin.
    readonly property bool tools: hovered || current
    readonly property alias picture: picture

    Accessible.role: Accessible.Button
    Accessible.name: entry.title
    Accessible.description: entry.summary + ", " + (entry.ready ? qsTr("Ready") : entry.needs)
                            + (entry.bookmarked ? ", " + qsTr("Pinned") : "")
    onTapped: card.chosen()

    ColumnLayout {
        anchors { fill: parent; margins: card.pad }
        spacing: card.u

        KitPicture {
            id: picture
            objectName: "appPicture-" + card.entry.id
            Layout.fillWidth: true
            Layout.preferredHeight: width * 10 / 16
            look: card.look
            source: card.entry.preview
            video: card.entry.video
            playing: card.playing
            radius: Math.max(2, card.look.params.cornerGroup * card.u - card.pad * 0.6)

            // Where its models are (#164): the same badges the Templates window draws.
            Rectangle {
                objectName: "appPlaces-" + card.entry.id
                anchors { left: parent.left; bottom: parent.bottom; margins: card.u }
                visible: card.entry.local || card.entry.hosts.length > 0 || card.entry.offlineHosts.length > 0
                width: placeRow.width + 2 * card.u
                height: 16 + card.u
                radius: height / 2
                color: card.look.body
                border.width: 1
                border.color: card.look.hairlineStrong
                Row {
                    id: placeRow
                    anchors.centerIn: parent
                    spacing: card.u
                    PlaceBadge { visible: card.entry.local; look: card.look; kind: "local" }
                    PlaceBadge {
                        visible: card.entry.hosts.length > 0 || card.entry.offlineHosts.length > 0
                        look: card.look
                        kind: "host"
                        hosts: card.entry.hosts.length > 0 ? card.entry.hosts : card.entry.offlineHosts
                        offline: card.entry.hosts.length === 0
                    }
                }
            }
            Row {
                anchors { right: parent.right; top: parent.top; margins: card.u * 1.5 }
                spacing: card.u
                KitChip {
                    objectName: "appBookmark-" + card.entry.id
                    visible: card.tools || card.entry.bookmarked
                    look: card.look
                    round: true
                    frosted: true
                    backdrop: picture.backdropItem
                    backdropSource: card.entry.preview
                    glyph: "pin"
                    current: card.entry.bookmarked
                    tip: card.entry.bookmarked ? qsTr("Unpin: let it take its place again") : qsTr("Pin to the top of the shelf")
                    Accessible.name: card.entry.bookmarked ? qsTr("Unpin %1").arg(card.entry.title) : qsTr("Pin %1").arg(card.entry.title)
                    onTapped: card.bookmarkToggled()
                }
                KitChip {
                    id: more
                    objectName: "appMore-" + card.entry.id
                    visible: card.tools
                    look: card.look
                    round: true
                    frosted: true
                    backdrop: picture.backdropItem
                    backdropSource: card.entry.preview
                    glyph: "ellipsis"
                    tip: qsTr("More: duplicate or rename")
                    Accessible.name: qsTr("More about %1").arg(card.entry.title)
                    onTapped: card.menuRequested(more)
                }
            }
        }
        Text {
            objectName: "appTitle-" + card.entry.id
            visible: !card.renaming
            Layout.fillWidth: true
            text: card.entry.title
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            color: card.look.text
            font.pixelSize: card.look.textSize
            font.weight: Font.DemiBold
        }
        Item { // the title as a field while it is renamed
            visible: card.renaming
            Layout.fillWidth: true
            Layout.preferredHeight: 7 * card.u
            KitWell { look: card.look; focused: nameInput.activeFocus }
            TextInput {
                id: nameInput
                objectName: "appRename-" + card.entry.id
                anchors { left: parent.left; right: parent.right; leftMargin: 2 * card.u; rightMargin: 2 * card.u
                          verticalCenter: parent.verticalCenter }
                clip: true
                selectByMouse: true
                text: card.entry.title
                color: card.look.text
                selectionColor: card.look.accent
                selectedTextColor: card.look.accentForeground
                font.pixelSize: card.look.textSize
                Accessible.role: Accessible.EditableText
                Accessible.name: qsTr("Name of the app")
                onVisibleChanged: if (visible) { text = card.entry.title; selectAll(); forceActiveFocus() }
                Keys.onReturnPressed: card.renamed(text)
                Keys.onEnterPressed: card.renamed(text)
                Keys.onEscapePressed: card.renameCancelled()
                onActiveFocusChanged: if (!activeFocus && card.renaming) card.renameCancelled()
            }
        }
        Text {
            objectName: "appSummary-" + card.entry.id
            Layout.fillWidth: true
            Layout.fillHeight: true
            text: card.entry.summary
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            color: card.look.label
            font.pixelSize: card.look.labelSize
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: card.u
            Glyph {
                visible: card.entry.ready
                look: card.look
                name: "check"
                color: card.look.label
                font.pixelSize: 16
            }
            Text {
                objectName: "appReady-" + card.entry.id
                Layout.fillWidth: true
                text: card.entry.ready ? qsTr("Ready") : card.entry.needs
                elide: Text.ElideRight
                color: card.look.label
                font.pixelSize: card.look.labelSize
                font.weight: card.entry.ready ? Font.Normal : Font.DemiBold
                ToolTip.visible: needsHover.hovered && card.entry.detail !== ""
                ToolTip.delay: card.look.tooltipDelay
                ToolTip.text: card.entry.detail
                HoverHandler { id: needsHover }
            }
        }
    }
}
