pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// A notice of the Drift kit (docs/canvas-look.md 20.23; docs/ui-ergonomics.md 2026-11-27): what happened, or what needs a look, said
// once, in a quiet card with the colour of what it is about. One component for every notice of the window: the workflow banner, the
// helper's offer, the keymap's problems, the Canvas view's toast and notes, the layers panel's view states, the app window's line.
//
//  - `tone`: info (blue), attention (yellow), error (red), success (green), neutral (the label colour). The card is the tone's quiet
//    surface and edge and the glyph its ink, from the theme's role tokens (look.role: `tint`, stepped until the text reads on it at
//    4.5:1, `frameEdge` and `frameTitle`, derived from the Omarchy palette, light and dark); never a colour of its own. Every word on
//    it is in the text colour (the label colour reads under 4.5:1 on some dark tints). Neutral has no glyph.
//  - `title`: one short line. `body`: an optional paragraph under it. `lines`: an optional list under it, each a string or a swap of
//    "Use my file" ({mine, theirs, minePath, tier}: the person's file, then the model it stands in for, then a quiet chip with the
//    tier), drawn as a small table whose names are cut in the middle, the whole name in a tooltip.
//  - `actions`: a Component put at the end of the title's line (Undo, Get ready…); `extra`: a Component under the lines (a download
//    under way, a row of buttons). Components, so a hidden notice is two items (#5: the window keeps few items).
//  - `copyText`: when set, a copy button (KitCopyButton) copies it, the whole message. `dismissible`: an × that says `dismissTip`.
//    `dismissed()` is the host's to act on: the notice never hides itself.
//  - `compact`: one line at caption size, for a view state in a narrow panel ("Showing Sky alone").
//
// Screen readers hear it as an alert (Accessible.AlertMessage, its title and body), announced once when its words come, not again
// when a panel above it opens again; `announceChanges: false` (a percent going up) keeps a change of words quiet. Tab reaches its
// buttons in reading order; Escape, while the focus is inside it, dismisses it (it takes the key before the window's shortcuts) and
// gives the focus to `returnFocus` when that is shown, else to the nearest item above it that takes Tab, so the keyboard is never
// left on a card that went away. It mirrors right to left with the window.
Item {
    id: notice
    property CanvasLook look: App.theme.look
    property string tone: "neutral"
    property string title
    property string body
    property var lines: []
    property string copyText
    property bool dismissible: true
    property string dismissTip: qsTr("Dismiss")
    property bool compact: false
    property Component actions: null
    property Component extra: null
    property Item returnFocus: null
    property bool announceChanges: true // false: a change of words while it shows is not announced again (a percent going up)
    property int announcements: 0       // how many times it was announced (tests)
    property string heard               // the words last announced; "" once they are gone
    // The inner parts' names, for tests and tools: by default the notice's own name with a suffix.
    property string titleName: objectName + "Title"
    property string bodyName: objectName + "Body"
    property string copyName: objectName + "Copy"
    property string dismissName: objectName + "Dismiss"
    signal dismissed()

    readonly property string roleName: ({ info: "blue", attention: "yellow", error: "error", success: "green" })[tone] ?? "muted"
    readonly property var role: { void notice.look.text; return notice.look.role(notice.roleName) } // read again when the theme changes
    readonly property string glyph: ({ info: "info", attention: "triangle-alert", error: "circle-x", success: "check" })[tone] ?? ""
    readonly property string spoken: [title, body].concat(lines.map(l => typeof l === "string" ? l
                                         : (l.tier ?? "") === "" ? qsTr("%1 in place of %2").arg(l.mine).arg(l.theirs)
                                         : qsTr("%1 in place of %2, %3").arg(l.mine).arg(l.theirs).arg(l.tier))).filter(t => t !== "").join("\n")
    // The lines the table draws, replaced only when they really change: a host's list is a new array at every change of its
    // owner (an edit, a tab shown), and a new model would make the rows again and drop a tooltip under the pointer.
    readonly property string linesKey: JSON.stringify(lines)
    property var drawnLines: []
    onLinesKeyChanged: drawnLines = lines
    Component.onCompleted: drawnLines = lines

    implicitHeight: card.item ? (card.item as Item).implicitHeight : 0
    height: implicitHeight
    // Right to left with the application, also where no mirrored root is above it (a popup, the app's own window).
    LayoutMirroring.enabled: Application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    // Where the focus goes when the notice goes: `returnFocus` when it is shown, else the nearest item above that takes Tab (a panel,
    // the canvas), else the window.
    function focusTarget() {
        if (notice.returnFocus && notice.returnFocus.visible)
            return notice.returnFocus
        for (let p = notice.parent; p; p = p.parent)
            if (p.visible && p.activeFocusOnTab)
                return p
        return notice.Window.contentItem
    }
    function dismiss() {
        let inside = false
        for (let p = notice.Window.activeFocusItem; p; p = p.parent)
            if (p === notice)
                inside = true
        if (inside)
            notice.focusTarget()?.forceActiveFocus()
        notice.dismissed()
    }
    // Once per message: when its words come while it shows, or it shows with words not yet heard (not when a panel above it opens
    // again on the same words). Weighed once the bindings have settled (a title and its lines change one after the other), and said
    // through the card, the item screen readers know as the alert.
    function listen() { Qt.callLater(notice.speak) }
    function speak() {
        if (notice.spoken === "") {
            notice.heard = ""
            return
        }
        if (!notice.visible || !card.item || notice.spoken === notice.heard || (!notice.announceChanges && notice.heard !== ""))
            return
        notice.heard = notice.spoken
        const alert = card.item as Item
        alert.Accessible.announce(notice.spoken)
        ++notice.announcements
    }
    onVisibleChanged: listen()
    onSpokenChanged: listen()
    // Escape is the window's too (Cancel the run, leave the group: CommandUi's shortcuts): the notice takes it first while the focus is
    // in it, as every other Escape of the window does.
    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape && notice.dismissible
    Keys.onEscapePressed: (event) => {
        if (notice.dismissible)
            notice.dismiss()
        else
            event.accepted = false
    }

    Loader { // made only while shown: a hidden notice is two items
        id: card
        width: notice.width
        active: notice.visible
        sourceComponent: Rectangle {
            id: surface
            readonly property real u: notice.look.unit
            readonly property real padX: (notice.compact ? 2 : 3) * u
            readonly property real padY: (notice.compact ? 1 : 2) * u
            readonly property bool panes: notice.look.params.areaLayout === "panes"
            // The head's first line is as tall as its buttons (7u; 6u compact): the glyph, the title's first line and each button
            // centre on it, so a title that wraps keeps its glyph and buttons on its first line.
            readonly property real line: (notice.compact ? 6 : 7) * u
            implicitHeight: column.implicitHeight + 2 * padY
            color: notice.role.tint // the tone's tint, stepped until the text reads on it at 4.5:1 (look.cpp roleTokens)
            border.width: 1
            border.color: notice.role.frameEdge
            radius: panes ? 0 : Math.min(notice.look.params.cornerGroup * u, 0.5 * height)
            Accessible.role: Accessible.AlertMessage
            Accessible.name: notice.title
            Accessible.description: notice.spoken

            ColumnLayout {
                id: column
                x: surface.padX
                y: surface.padY
                width: surface.width - 2 * surface.padX
                spacing: surface.u

                RowLayout {
                    id: head
                    Layout.fillWidth: true
                    visible: notice.title !== "" || notice.body !== "" || notice.dismissible || notice.copyText !== ""
                    spacing: 2 * surface.u
                    Glyph {
                        id: mark
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: Math.max(0, (surface.line - implicitHeight) / 2)
                        visible: notice.glyph !== "" && (notice.title !== "" || notice.body !== "")
                        look: notice.look
                        name: notice.glyph
                        color: notice.role.frameTitle
                        Accessible.ignored: true
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: titleText.visible ? Math.max(0, (surface.line - titleMetrics.height) / 2) : 0
                        spacing: surface.u / 2
                        FontMetrics { id: titleMetrics; font: titleText.font }
                        Text {
                            id: titleText
                            objectName: notice.titleName
                            Layout.fillWidth: true
                            visible: text !== ""
                            text: notice.title
                            textFormat: Text.PlainText
                            wrapMode: notice.compact ? Text.NoWrap : Text.Wrap
                            elide: notice.compact ? Text.ElideRight : Text.ElideNone
                            color: notice.look.text
                            font.pixelSize: notice.compact ? notice.look.captionSize : notice.look.textSize
                            font.weight: notice.compact || notice.body === "" && notice.drawnLines.length === 0 ? Font.Normal : Font.DemiBold
                        }
                        Text {
                            objectName: notice.bodyName
                            Layout.fillWidth: true
                            visible: text !== ""
                            text: notice.body
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: notice.look.text
                            font.pixelSize: notice.look.labelSize
                        }
                    }
                    Loader {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: item ? Math.max(0, (surface.line - (item as Item).height) / 2) : 0
                        active: notice.actions !== null
                        sourceComponent: notice.actions
                    }
                    Loader {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: item ? Math.max(0, (surface.line - (item as Item).height) / 2) : 0
                        active: notice.copyText !== ""
                        sourceComponent: KitCopyButton {
                            objectName: notice.copyName
                            look: notice.look
                            copyText: notice.copyText
                        }
                    }
                    Loader {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: item ? Math.max(0, (surface.line - (item as Item).height) / 2) : 0
                        active: notice.dismissible
                        sourceComponent: ViewButton {
                            objectName: notice.dismissName
                            look: notice.look
                            glyph: "x" // the Lucide x every other close and dismiss draws (canvas-look 16)
                            flat: true
                            compact: notice.compact
                            tip: notice.dismissTip
                            onTapped: notice.dismiss()
                        }
                    }
                }

                // The lines: plain sentences, or the swaps as a small table (yours, "in place of", theirs, the tier), its columns as
                // wide as their longest name, shared out when the card is narrower, each name cut in the middle. When the names would
                // have less than 24u each beside the chip (a narrow panel), the chip goes under them.
                Column {
                    id: list
                    Layout.fillWidth: true
                    Layout.leftMargin: mark.visible ? mark.width + 2 * surface.u : 0
                    visible: notice.drawnLines.length > 0
                    spacing: surface.u
                    FontMetrics { id: nameMetrics; font.pixelSize: notice.look.textSize; font.weight: Font.Medium }
                    FontMetrics { id: plainMetrics; font.pixelSize: notice.look.textSize }
                    readonly property var swaps: notice.drawnLines.filter(l => typeof l !== "string")
                    readonly property real joinWidth: joinProbe.implicitWidth
                    readonly property real chipWidth: Math.max(0, ...swaps.map(l => plainMetrics.advanceWidth(l.tier ?? "") * notice.look.labelSize / notice.look.textSize))
                                                      + 4 * surface.u
                    readonly property bool stacked: width - joinWidth - chipWidth - 6 * surface.u < 48 * surface.u
                    readonly property real room: Math.max(0, width - joinWidth - 4 * surface.u - (stacked ? 0 : chipWidth + 2 * surface.u))
                    readonly property real mineWanted: Math.max(0, ...swaps.map(l => Math.ceil(nameMetrics.advanceWidth(l.mine)) + 1))
                    readonly property real theirsWanted: Math.max(0, ...swaps.map(l => Math.ceil(plainMetrics.advanceWidth(l.theirs)) + 1))
                    readonly property real mineColumn: Math.min(mineWanted, Math.max(room / 2, room - theirsWanted))
                    readonly property real theirsColumn: Math.min(theirsWanted, room - mineColumn)
                    Text {
                        id: joinProbe
                        visible: false
                        text: qsTr("in place of")
                        font.pixelSize: notice.look.labelSize
                    }
                    Repeater {
                        id: lineRows
                        objectName: "noticeLines"
                        model: notice.drawnLines
                        delegate: Loader {
                            id: line
                            required property var modelData
                            width: list.width
                            sourceComponent: typeof modelData === "string" ? plainLine : swapLine
                            Component {
                                id: plainLine
                                Text {
                                    text: line.modelData
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    color: notice.look.text
                                    font.pixelSize: notice.look.labelSize
                                }
                            }
                            Component {
                                id: swapLine
                                Column {
                                spacing: surface.u / 2
                                Row {
                                    spacing: 2 * surface.u
                                    Text {
                                        objectName: "noticeLineMine"
                                        width: list.mineColumn
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: line.modelData.mine
                                        textFormat: Text.PlainText
                                        elide: Text.ElideMiddle
                                        color: notice.look.text
                                        font.pixelSize: notice.look.textSize
                                        font.weight: Font.Medium
                                        HoverHandler { id: mineHover }
                                        ToolTip.visible: mineHover.hovered
                                        ToolTip.delay: notice.look.tooltipDelay
                                        ToolTip.text: qsTr("Your file: %1").arg(line.modelData.minePath || line.modelData.mine)
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: qsTr("in place of")
                                        color: notice.look.text // smaller, not paler: the label colour reads under 4.5:1 on some dark tints
                                        font.pixelSize: notice.look.labelSize
                                    }
                                    Text {
                                        objectName: "noticeLineTheirs"
                                        width: list.theirsColumn
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: line.modelData.theirs
                                        textFormat: Text.PlainText
                                        elide: Text.ElideMiddle
                                        color: notice.look.text
                                        font.pixelSize: notice.look.textSize
                                        HoverHandler { id: theirsHover }
                                        ToolTip.visible: theirsHover.hovered && parent.visible
                                        ToolTip.delay: notice.look.tooltipDelay
                                        ToolTip.text: qsTr("In place of: %1").arg(line.modelData.theirs)
                                    }
                                    KitBadge {
                                        objectName: "noticeLineTier"
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: text !== "" && !list.stacked
                                        look: notice.look
                                        text: line.modelData.tier ?? ""
                                    }
                                }
                                KitBadge {
                                    objectName: "noticeLineTier"
                                    visible: text !== "" && list.stacked
                                    look: notice.look
                                    text: line.modelData.tier ?? ""
                                }
                                }
                            }
                        }
                    }
                }

                Loader {
                    Layout.fillWidth: true
                    Layout.leftMargin: list.Layout.leftMargin
                    active: notice.extra !== null
                    visible: active
                    sourceComponent: notice.extra
                }
            }
        }
    }
}
