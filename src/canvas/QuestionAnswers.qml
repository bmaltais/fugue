import QtQuick
import QtQuick.Layouts
import Fugue

// The row of answers at the foot of a Question (#235): the answers are ThemedButtons (ViewButton), Cancel or the safe answer
// first and the primary last at the right (D9). Right to left it mirrors with the question's content. A destructive answer
// stands in its own chunk: either last, after the wider gap (`spacing: 4 * unit`, twice the row's), or alone at the far side
// with `leadingSpacer: false` and an `Item { Layout.fillWidth: true }` after it.
RowLayout {
    id: answers
    property bool leadingSpacer: true
    Layout.fillWidth: true
    spacing: 2 * App.theme.look.unit
    Item {
        visible: answers.leadingSpacer
        Layout.fillWidth: true
    }
}
