import QtQuick
import "../canvas"
import "../kit"

// "Community · by <author>" (spec 11.10): the mark of a template a user submitted to fugue-workflows, a frosted kit chip over a card's
// picture. Its tooltip says who sent it; with an issue (`url`) a press opens it in the browser, as the details' source line opens its link.
KitChip {
    id: chip
    property string author
    property string url
    frosted: true
    pill: true
    text: author !== "" ? qsTr("Community · by %1").arg(author) : qsTr("Community")
    tip: url !== "" ? qsTr("Submitted by a user to the community templates. Press to open its issue: %1").arg(url)
                    : qsTr("Submitted by a user to the community templates.")
    Accessible.name: text
    onTapped: if (url !== "") Qt.openUrlExternally(url)
}
