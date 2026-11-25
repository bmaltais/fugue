pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Fugue
import "../canvas"

// The run history strip of an app ("takes", #389): the thumbnails of this app's runs this session, newest first, the shown one
// outlined, a badge on one that waits or goes, a mark on one that failed. A press shows that take in the stage (AppView.selectTake);
// Left and Right move, Enter shows and R runs again with the inputs of the one with the place. The takes of an app with a file come
// from History (they survive a restart); one read back asks for its results only when it is drawn (AppView.ensureTake), so a long
// history loads nothing it does not show. It scrolls sideways when there are more than fit. It knows nothing of runs: it draws
// `appView.takes`.
ListView {
    id: strip
    objectName: "appTakes"
    required property AppView appView
    required property CanvasLook look
    readonly property real size: 14 * look.unit

    implicitHeight: size + look.unit + (ScrollBar.horizontal as FugueScrollBar).inset
    orientation: ListView.Horizontal
    spacing: look.unit
    clip: true
    model: appView.takes
    activeFocusOnTab: true
    keyNavigationEnabled: true
    highlightFollowsCurrentItem: true
    boundsBehavior: Flickable.StopAtBounds
    Accessible.role: Accessible.List
    Accessible.name: qsTr("Takes")
    Keys.onReturnPressed: appView.selectTake(currentIndex)
    Keys.onEnterPressed: appView.selectTake(currentIndex)
    Keys.onSpacePressed: appView.selectTake(currentIndex)
    // R: run again with the inputs of the take that has the place (the toolbar's Run again acts on the shown one).
    Keys.onPressed: event => {
        if (event.key === Qt.Key_R && event.modifiers === Qt.NoModifier && currentIndex >= 0) {
            appView.runAgain(currentIndex)
            event.accepted = true
        }
    }
    ScrollBar.horizontal: FugueScrollBar { look: strip.look }

    delegate: Rectangle {
        id: take
        required property var modelData
        required property int index
        readonly property bool failed: modelData.status === "failed"
        // The picture's own shape at the strip's height (owner, 2026-11-25: takes were all square crops), kept between half a square
        // and two and a half; a square until the picture has loaded, and for a take with no picture.
        readonly property real aspect: thumb.status === Image.Ready && thumb.implicitHeight > 0 ? thumb.implicitWidth / thumb.implicitHeight : 1
        width: Math.round(strip.size * Math.max(0.5, Math.min(2.5, aspect)))
        height: strip.size
        radius: strip.look.params.cornerWell * strip.look.unit
        color: strip.look.kitChip
        border.width: modelData.shown || (strip.activeFocus && ListView.isCurrentItem) ? 2 : 0
        border.color: strip.look.accent
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Take %1").arg(strip.appView.takes.length - index)
        Accessible.description: modelData.line
        Component.onCompleted: strip.appView.ensureTake(index)
        ToolTip.visible: hover.hovered && modelData.line !== ""
        ToolTip.delay: strip.look.tooltipDelay
        ToolTip.text: modelData.line
        Image {
            id: thumb
            anchors { fill: parent; margins: 2 }
            visible: take.modelData.url !== ""
            source: take.modelData.url
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            sourceSize.height: 128
            opacity: take.modelData.pending ? 0.5 : 1
        }
        Glyph {
            anchors.centerIn: parent
            visible: take.modelData.url === "" && !take.modelData.pending
            look: strip.look
            name: take.failed ? "triangle-alert" : "image"
            color: take.failed ? strip.look.error : strip.look.label
            font.pixelSize: 16
        }
        // The pending badge: a turning mark in a dot at the corner.
        Rectangle {
            visible: take.modelData.pending
            objectName: "appTakePending"
            anchors { top: parent.top; right: parent.right; margins: strip.look.unit / 2 }
            width: 5 * strip.look.unit
            height: width
            radius: width / 2
            color: strip.look.accent
            Glyph {
                id: spinner
                anchors.centerIn: parent
                look: strip.look
                name: "loader-circle"
                color: strip.look.accentForeground
                font.pixelSize: 11
                RotationAnimator on rotation { running: take.modelData.pending && take.visible; from: 0; to: 360; duration: 1200; loops: Animation.Infinite }
            }
        }
        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: { strip.currentIndex = take.index; strip.appView.selectTake(take.index) } }
    }
}
