pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// One app on the shelf (#388): its picture, its name, one line of what it does, and one line of whether it is ready: a check and
// "Ready", or what it still needs ("Needs 2.1 GB and 1 pack"). The whole card is the one button; choosing opens the app, and an
// app that is not ready asks for what it needs once, on the one Get ready sheet. The Templates window's card, smaller: no
// details chip and no download button, which are for browsing, not for starting (docs/ui-ergonomics.md, "Apps shelf").
//
// `entry` is one entry of AppShelf.apps (apps/app_shelf.h). The owner sets `current` (the grid's focused card) and listens to
// `chosen`.
Rectangle {
    id: card

    required property CanvasLook look
    required property var entry // the shelf's entry: {id, title, summary, preview, ready, needs, detail, local, hosts, offlineHosts}
    property bool current: false
    signal chosen
    signal hovered

    readonly property real u: look.unit

    radius: look.corner
    color: hover.hovered || current ? look.well : "transparent"
    border.width: current ? 2 : 0
    border.color: look.accent
    Accessible.role: Accessible.Button
    Accessible.name: entry.title
    Accessible.description: entry.summary + ", " + (entry.ready ? qsTr("Ready") : entry.needs)
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor; onHoveredChanged: if (hovered) card.hovered() }
    TapHandler { onTapped: card.chosen() }

    ColumnLayout {
        anchors { fill: parent; margins: 2 * card.u }
        spacing: card.u

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: width * 10 / 16
            radius: card.look.thumbCorner
            color: card.look.well
            clip: true
            Image {
                anchors.fill: parent
                source: card.entry.preview
                visible: source !== ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                sourceSize.width: 512
            }
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
        }
        Text {
            objectName: "appTitle-" + card.entry.id
            Layout.fillWidth: true
            text: card.entry.title
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            color: card.look.text
            font.pixelSize: card.look.textSize
            font.weight: Font.DemiBold
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
