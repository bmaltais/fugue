pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../canvas"

// The second, separate confirmation before a group is deleted from the user's library (#156). Its
// own question, as docs/ui-ergonomics.md asks of a destructive step: an accent border, the group
// named, what happens said in full, and Cancel (the default: Enter and Escape keep the group)
// before Delete. It only asks; `confirmed` says yes, and what deleting does is the caller's
// (Settings > Packs and the node library share one call, PacksModel::removeLibraryGroup).
//
//   question.ask("local/fox-pass", "Fox pass")   opens it for that group
Question {
    id: root
    objectName: "deleteGroupQuestion"

    property string groupId: ""
    property string groupName: ""

    signal confirmed(string groupId, string groupName)

    function ask(id, name) {
        groupId = id
        groupName = name
        open()
    }

    onOpened: cancel.forceActiveFocus()

    background: Item {
        PopupSurface { anchors.fill: parent; look: root.look }
        // The accent border marks it as a question that needs an answer, not a notice.
        Rectangle {
            anchors.fill: parent
            radius: root.look.popupCorner
            color: "transparent"
            border.width: 2
            border.color: root.look.accent
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 3 * root.look.unit
        LayoutMirroring.enabled: Application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true
        Label {
            objectName: "deleteGroupTitle"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.bold: true
            text: qsTr("Delete the group “%1” from your library?").arg(root.groupName)
        }
        Label {
            objectName: "deleteGroupDetail"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: root.look.label
            text: qsTr("Only the copy in your library is deleted. Workflows that already use it keep their own copy and still open and run. This cannot be undone.")
        }
        // Delete is the destructive one: after Cancel, with the dialog's own gap before it.
        QuestionAnswers {
            Layout.topMargin: 2 * root.look.unit
            spacing: 4 * root.look.unit
            ThemedButton {
                id: cancel
                objectName: "deleteGroupCancel"
                text: qsTr("Cancel")
                highlighted: true
                focus: true
                onClicked: root.close()
            }
            ThemedButton {
                objectName: "deleteGroupConfirm"
                text: qsTr("Delete")
                danger: true
                onClicked: {
                    root.close()
                    root.confirmed(root.groupId, root.groupName)
                }
            }
        }
    }
}
