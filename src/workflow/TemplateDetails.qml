pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// A template's details, a sheet over the Templates dialog's grid (docs/ui-ergonomics.md, 2026-11-24): a banner cropped
// from its preview, the title and the whole description, where it comes from, the models it uses and whether
// each is here, the packs it needs, its sample inputs, and Open (the primary) and Cancel. `template` is one entry of
// Workflows::templates(). The dialog closes it on Escape; it shows it and puts the focus back on the card.
Rectangle {
    id: sheet
    objectName: "templateDetails"
    required property var template
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    signal open()
    signal cancel()

    color: look.popup
    radius: look.popupCorner
    focus: visible
    // Just after the sheet comes up, a press on it is the second click of a double click on the card: it opens the template.
    property bool settling: false
    onVisibleChanged: if (visible) forceActiveFocus()

    readonly property var source: template.source ? template.source : {}
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
                width: scroll.width - (scroll.ScrollBar.vertical as FugueScrollBar).breadth
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
                    TapHandler { enabled: sourceLine.link !== ""; onTapped: Qt.openUrlExternally(sourceLine.link) }
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
                                text: [row.modelData.kind, row.modelData.size, row.modelData.license].filter(p => p !== "").join(" · ")
                                elide: Text.ElideRight
                                color: sheet.look.label
                                font.pixelSize: sheet.look.labelSize
                            }
                        }
                        Text {
                            text: row.modelData.status === "disk" ? qsTr("On disk")
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
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 4 * sheet.u
            spacing: 2 * sheet.u
            Item { Layout.fillWidth: true }
            ThemedButton {
                objectName: "templateDetailsCancel"
                text: qsTr("Cancel")
                onClicked: sheet.cancel()
            }
            ThemedButton {
                objectName: "templateDetailsOpen"
                text: qsTr("Open")
                highlighted: true
                onClicked: sheet.open()
            }
        }
    }

    MouseArea {
        z: 10
        anchors.fill: parent
        enabled: sheet.settling
        onPressed: sheet.open()
    }
}
