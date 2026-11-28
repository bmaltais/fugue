pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"
import "../kit"

// A template's details, a sheet over the Templates dialog's grid (docs/ui-ergonomics.md, 2026-11-24): a banner cropped
// from its preview, the title and the whole description, where it comes from, the models it uses and whether
// each is here, the packs it needs, its sample inputs, and Open (the primary) and Cancel. `template` is one entry of
// Workflows::templates(). The dialog closes it on Escape; it shows it and puts the focus back on the card.
// A template ready only through the person's own files (#605, #616) says so on each model it stands in for ("Your file", and which),
// and its actions say what they do (docs/ui-ergonomics.md 2026-11-27): "Open with your files" is the primary; the template's own
// download is a secondary at the row's start, "Download the template's own files (8.4 GB)…", apart from Cancel and Open.
Rectangle {
    id: sheet
    objectName: "templateDetails"
    required property var template
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    signal open()
    signal downloadOwn()
    signal cancel()

    color: look.popup
    radius: look.popupCorner
    focus: visible
    onVisibleChanged: if (visible) forceActiveFocus()

    readonly property var source: template.source ? template.source : {}
    readonly property bool withFiles: template.opensWithFiles === true // ready only through the person's files: Open uses them
    // How many files of the person's Open uses (two models one file stands in for count once), by path as the banner counts them.
    readonly property int yourFiles: new Set((template.onDisk ?? []).map(d => d.path || d.file)).size
    // Enter takes the primary (docs/ui-ergonomics.md D5); a focused button takes its own Enter first.
    Keys.onReturnPressed: sheet.open()
    Keys.onEnterPressed: sheet.open()
    readonly property string sourceText: source.name && source.author ? qsTr("Based on %1 by %2").arg(source.name).arg(source.author)
                                         : source.name ? qsTr("Based on %1").arg(source.name)
                                         : source.author ? qsTr("Workflow by %1").arg(source.author) : ""

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The banner: the preview cropped wide, fading into the sheet.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: sheet.height / 3
            clip: true
            Image {
                anchors.fill: parent
                source: sheet.template.preview
                visible: source !== ""
                fillMode: Image.PreserveAspectCrop
                verticalAlignment: Image.AlignVCenter
                asynchronous: true
                sourceSize.width: 1200
            }
            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: parent.height / 2
                gradient: Gradient {
                    GradientStop { position: 0; color: "transparent" }
                    GradientStop { position: 1; color: sheet.look.popup }
                }
            }
        }

        Flickable {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 6 * sheet.u
            Layout.rightMargin: 6 * sheet.u
            clip: true
            contentWidth: width
            contentHeight: body.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FugueScrollBar { look: sheet.look }

            ColumnLayout {
                id: body
                x: Math.max(0, Math.floor((scroll.width - width) / 2))
                // A readable line (about 90 characters), not the sheet's whole width.
                width: Math.min(scroll.width - (scroll.ScrollBar.vertical as FugueScrollBar).breadth, 150 * sheet.u)
                spacing: 3 * sheet.u

                Text {
                    objectName: "templateDetailsTitle"
                    Layout.fillWidth: true
                    text: sheet.template.title
                    wrapMode: Text.Wrap
                    color: sheet.look.text
                    font.pixelSize: sheet.look.overlayTitleSize
                    font.weight: Font.DemiBold
                }
                Text {
                    objectName: "templateDetailsDescription"
                    Layout.fillWidth: true
                    text: sheet.template.description
                    wrapMode: Text.Wrap
                    color: sheet.look.text
                    font.pixelSize: sheet.look.textSize
                }
                Text {
                    id: sourceLine
                    objectName: "templateDetailsSource"
                    readonly property string link: sheet.source.url ? sheet.source.url : ""
                    Layout.fillWidth: true
                    visible: sheet.sourceText !== ""
                    text: sheet.sourceText + (sheet.source.note ? " · " + sheet.source.note : "")
                    wrapMode: Text.Wrap
                    color: link !== "" ? sheet.look.accent : sheet.look.label
                    font.pixelSize: sheet.look.textSize
                    font.underline: link !== "" && sourceHover.hovered
                    HoverHandler { id: sourceHover; cursorShape: sourceLine.link !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor }
                    TapHandler { gesturePolicy: TapHandler.WithinBounds; enabled: sourceLine.link !== ""; onTapped: Qt.openUrlExternally(sourceLine.link) }
                }
                Text {
                    id: communityLine
                    objectName: "templateDetailsCommunity"
                    readonly property string link: sheet.template.communityUrl ? sheet.template.communityUrl : ""
                    Layout.fillWidth: true
                    visible: sheet.template.community === true
                    text: sheet.template.communityAuthor ? qsTr("Community template, submitted by %1").arg(sheet.template.communityAuthor)
                                                         : qsTr("Community template, submitted by a user")
                    wrapMode: Text.Wrap
                    color: link !== "" ? sheet.look.accent : sheet.look.label
                    font.pixelSize: sheet.look.textSize
                    font.underline: link !== "" && communityHover.hovered
                    HoverHandler { id: communityHover; cursorShape: communityLine.link !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor }
                    TapHandler { gesturePolicy: TapHandler.WithinBounds; enabled: communityLine.link !== ""; onTapped: Qt.openUrlExternally(communityLine.link) }
                }
                Text {
                    objectName: "templateDetailsFacts"
                    Layout.fillWidth: true
                    readonly property var parts: [sheet.template.categoryTitle,
                                                  (sheet.template.families || []).join(", "),
                                                  (sheet.template.tags || []).join(", ")].filter(p => p !== "")
                    visible: parts.length > 0
                    text: parts.join("  ·  ")
                    wrapMode: Text.Wrap
                    color: sheet.look.label
                    font.pixelSize: sheet.look.textSize
                }

                Text {
                    visible: sheet.template.modelList.length > 0
                    text: qsTr("Models used")
                    color: sheet.look.text
                    font.pixelSize: sheet.look.resultSize
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: sheet.template.modelList
                    delegate: RowLayout {
                        id: row
                        required property var modelData
                        // The person's file that stands in for this model, when there is one (Workflows::templates' onDisk).
                        // Only when Open will use it (opensUsingFiles: with "Ask me" a template partly covered opens with its own models).
                        readonly property var yours: sheet.template.opensUsingFiles === true
                                                     ? (sheet.template.onDisk ?? []).find(d => d.name === modelData.name) ?? null : null
                        objectName: "templateDetailsModel-" + modelData.name
                        Layout.fillWidth: true
                        spacing: 2 * sheet.u
                        Glyph {
                            look: sheet.look
                            name: row.modelData.status === "missing" ? "x" : "check"
                            color: row.modelData.status === "missing" ? sheet.look.error : sheet.look.accent
                            font.pixelSize: sheet.look.textSize
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text {
                                Layout.fillWidth: true
                                text: row.modelData.name
                                elide: Text.ElideMiddle
                                color: sheet.look.text
                                font.pixelSize: sheet.look.textSize
                            }
                            Text {
                                Layout.fillWidth: true
                                text: row.yours ? qsTr("Opens with your %1 (%2)").arg(row.yours.file).arg(row.yours.same)
                                      : [row.modelData.kind, row.modelData.size, row.modelData.license].filter(p => p !== "").join(" · ")
                                elide: row.yours ? Text.ElideMiddle : Text.ElideRight
                                color: sheet.look.label
                                font.pixelSize: sheet.look.labelSize
                            }
                        }
                        Text {
                            text: row.yours ? qsTr("Your file")
                                  : row.modelData.status === "disk" ? qsTr("On disk")
                                  : row.modelData.status === "served" ? qsTr("Served") : qsTr("Missing")
                            color: row.modelData.status === "missing" ? sheet.look.error : sheet.look.label
                            font.pixelSize: sheet.look.labelSize
                        }
                    }
                }

                Text {
                    visible: sheet.template.packs.length > 0
                    text: qsTr("Packs needed")
                    color: sheet.look.text
                    font.pixelSize: sheet.look.resultSize
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: sheet.template.packs
                    delegate: Text {
                        required property var modelData
                        objectName: "templateDetailsPack-" + modelData.id
                        Layout.fillWidth: true
                        text: modelData.title + " · " + (modelData.carried ? qsTr("carried by the template") : qsTr("from elsewhere"))
                              + " · " + (modelData.installed ? qsTr("installed") : qsTr("not installed"))
                        wrapMode: Text.Wrap
                        color: modelData.installed ? sheet.look.label : sheet.look.error
                        font.pixelSize: sheet.look.textSize
                    }
                }

                Text {
                    visible: sheet.template.inputs.length > 0
                    text: qsTr("Sample inputs")
                    color: sheet.look.text
                    font.pixelSize: sheet.look.resultSize
                    font.weight: Font.DemiBold
                }
                Flow {
                    Layout.fillWidth: true
                    visible: sheet.template.inputs.length > 0
                    spacing: 2 * sheet.u
                    Repeater {
                        model: sheet.template.inputs
                        delegate: Image {
                            required property string modelData
                            width: 20 * sheet.u
                            height: 14 * sheet.u
                            source: modelData
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            sourceSize.width: 160
                        }
                    }
                }
                Item { Layout.preferredHeight: sheet.u }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: sheet.look.hairline }
        KitDialogButtons {
            id: actionRow
            Layout.margins: 4 * sheet.u
            // The template's own models, for anyone who wants them in place of their files (opens Get ready with Download chosen): a
            // secondary that names what it fetches, at the row's start, apart from Cancel and Open, so it never reads as the way to open.
            KitChip {
                objectName: "templateDetailsDownload"
                dialogRole: "extra"
                look: sheet.look
                bordered: true
                visible: sheet.withFiles && sheet.template.ownDownloadSize !== ""
                text: qsTr("Download the template's own files (%1)…").arg(sheet.template.ownDownloadSize)
                // A narrow sheet cuts the label, never Cancel and Open: the whole label is in the tip.
                maxTextWidth: Math.max(12 * sheet.u, actionRow.width - cancelButton.width - openButton.width - 13 * sheet.u)
                tip: qsTr("%1: the exact files the template was made with, in place of yours. Get ready shows what it fetches before anything starts.").arg(text)
                onTapped: sheet.downloadOwn()
            }
            ThemedButton {
                id: cancelButton
                objectName: "templateDetailsCancel"
                dialogRole: "cancel"
                text: qsTr("Cancel")
                onClicked: sheet.cancel()
            }
            ThemedButton {
                id: openButton
                objectName: "templateDetailsOpen"
                text: sheet.withFiles ? qsTr("Open with your %n files", "", sheet.yourFiles) : qsTr("Open")
                highlighted: true
                onClicked: sheet.open()
            }
        }
    }
}
