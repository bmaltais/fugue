import QtQuick

// The run pill, top right of the canvas (docs/canvas-look.md 11): while a run of the window
// is waiting or going, a spinner, "Running", the node and its percent, and "1 of 3" when
// more are queued; after a run with errors, their count. A click lists the errors with the
// path id of each node (spec 12.7: "an error panel with the node's path id"); a click on one
// selects the node it shows on.
Item {
    id: pill
    required property var look
    required property var runs // CanvasRuns
    readonly property real u: look.unit
    readonly property int errorCount: runs.errors.length
    property bool listOpen: false

    visible: runs.busy || errorCount > 0
    width: box.width
    height: box.height + (list.visible ? list.height + u : 0)
    onErrorCountChanged: if (errorCount === 0) listOpen = false

    function percent(p) { return p >= 0 ? " " + Math.round(p * 100) + "%" : "" }

    Shadow {
        target: box
        level: 2
        look: pill.look
    }
    Rectangle {
        id: box
        objectName: "runPillBox"
        anchors.right: parent.right
        width: row.implicitWidth + 6 * pill.u
        height: 7 * pill.u
        radius: Math.min(pill.look.popupCorner, 14)
        color: pill.look.popup
        border.width: 1
        border.color: pill.errorCount > 0 && !pill.runs.busy ? pill.look.error : pill.look.hairline

        Row {
            id: row
            anchors.centerIn: parent
            spacing: 1.5 * pill.u
            Glyph {
                anchors.verticalCenter: parent.verticalCenter
                look: pill.look
                name: pill.runs.busy ? "loader-circle" : "triangle-alert"
                color: pill.runs.busy ? pill.look.accent : pill.look.error
                RotationAnimator on rotation {
                    running: pill.runs.running
                    loops: Animation.Infinite
                    from: 0
                    to: 360
                    duration: 900
                }
            }
            Text {
                objectName: "runPillText"
                anchors.verticalCenter: parent.verticalCenter
                text: pill.runs.running ? (pill.runs.activeNode !== ""
                                           ? qsTr("Running %1").arg(pill.runs.activeNode) + pill.percent(pill.runs.progress)
                                           : qsTr("Running"))
                      : pill.runs.busy ? qsTr("Waiting for the GPU")
                      : pill.errorCount === 1 ? qsTr("1 error") : qsTr("%1 errors").arg(pill.errorCount)
                textFormat: Text.PlainText
                color: pill.look.text
                font.pixelSize: pill.look.labelSize
                font.features: { "tnum": 1 }
            }
            Text {
                visible: pill.runs.count > 1
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("1 of %1").arg(pill.runs.count)
                color: pill.look.label
                font.pixelSize: pill.look.labelSize
                font.features: { "tnum": 1 }
            }
        }
        MouseArea {
            anchors.fill: parent
            enabled: pill.errorCount > 0
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: pill.listOpen = !pill.listOpen
        }
    }

    // The error list: one row per error, the path id first.
    Rectangle {
        id: list
        objectName: "runErrors"
        visible: pill.listOpen && pill.errorCount > 0
        anchors.right: parent.right
        y: box.height + pill.u
        width: 90 * pill.u
        height: column.implicitHeight + 2 * pill.u
        radius: Math.min(pill.look.popupCorner, 14)
        color: pill.look.popup
        border.width: 1
        border.color: pill.look.hairline
        Column {
            id: column
            x: pill.u
            y: pill.u
            width: parent.width - 2 * pill.u
            Repeater {
                model: list.visible ? pill.runs.errors : []
                delegate: Rectangle {
                    id: entry
                    required property var modelData
                    width: column.width
                    height: entryText.implicitHeight + 2 * pill.u
                    radius: pill.look.smallCorner
                    color: entryArea.containsMouse ? pill.look.errorChip : "transparent"
                    Rectangle {
                        width: 2
                        height: parent.height
                        color: pill.look.error
                    }
                    Text {
                        id: entryText
                        x: 2 * pill.u
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 3 * pill.u
                        text: "<b>" + entry.modelData.node + "</b> " + entry.modelData.message
                        textFormat: Text.StyledText
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        color: pill.look.text
                        font.pixelSize: pill.look.labelSize
                    }
                    MouseArea {
                        id: entryArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: pill.runs.showError(entry.modelData.shown)
                    }
                }
            }
        }
    }
}
