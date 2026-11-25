pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "../canvas"

// Results and pictures (the rail's second item; ComfyUI's Media Assets, layout only): a popover with a search field and a grid
// of tiles. "This app's results" are the runs of the session (a tile shows that take in the stage), "Pictures" the recent ones
// of the inputs folder (a tile becomes the app's picture input). Escape closes it. AppView does the work.
Popup {
    id: pop
    objectName: "appLibrary"
    required property AppView appView
    required property CanvasLook look
    readonly property real u: look.unit
    property string query: ""
    readonly property string imageKey: appView.imageKey()

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(120 * u, (parent ? parent.width : 900) - 12 * u)
    height: Math.min(150 * u, (parent ? parent.height : 700) - 12 * u)
    Overlay.modal: Rectangle { color: pop.look.scrim }
    background: PopupSurface { look: pop.look }
    enter: null
    exit: null
    onAboutToShow: { query = ""; search.text = ""; recents = appView.recentImages() }
    onOpened: search.forceFocus()
    property var recents: []

    function matches(name) { return query === "" || name.toLowerCase().includes(query.toLowerCase()) }
    // Built only while it is open: a closed library holds no pictures.
    readonly property var takeTiles: !visible ? [] : appView.takes.map((t, i) => ({take: i, url: t.url, name: t.line !== "" ? t.line : qsTr("Take %1").arg(appView.takes.length - i),
                                                                  pending: t.pending, shown: t.shown}))
                                                   .filter(t => t.url !== "" && matches(t.name))
    readonly property var pictureTiles: !visible ? [] : recents.filter(r => matches(r.name))

    contentItem: ColumnLayout {
        spacing: 2 * pop.u
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 3 * pop.u
            Layout.bottomMargin: 0
            Text {
                text: qsTr("Results and pictures")
                color: pop.look.text
                font.pixelSize: pop.look.overlayTitleSize
                font.weight: Font.DemiBold
            }
            AppField {
                id: search
                objectName: "appLibrarySearch"
                Layout.fillWidth: true
                look: pop.look
                placeholder: qsTr("Search")
                label: qsTr("Search")
                onTextEdited: pop.query = text
            }
            ViewButton {
                look: pop.look
                glyph: "x"
                flat: true
                tip: qsTr("Close")
                onTapped: pop.close()
            }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 3 * pop.u
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: parent.width
                spacing: 2 * pop.u
                Text {
                    visible: pop.takeTiles.length > 0
                    text: qsTr("This app's results")
                    color: pop.look.label
                    font.pixelSize: pop.look.labelSize
                }
                Flow {
                    Layout.fillWidth: true
                    visible: pop.takeTiles.length > 0
                    spacing: 2 * pop.u
                    Repeater {
                        model: pop.takeTiles
                        delegate: Tile {
                            required property var modelData
                            objectName: "appLibraryTake"
                            url: modelData.url
                            name: modelData.name
                            chosen: modelData.shown
                            onTapped: { pop.appView.selectTake(modelData.take); pop.close() }
                        }
                    }
                }
                Text {
                    visible: pop.pictureTiles.length > 0
                    text: pop.imageKey !== "" ? qsTr("Pictures: press one to use it") : qsTr("Pictures")
                    color: pop.look.label
                    font.pixelSize: pop.look.labelSize
                }
                Flow {
                    Layout.fillWidth: true
                    visible: pop.pictureTiles.length > 0
                    spacing: 2 * pop.u
                    Repeater {
                        model: pop.pictureTiles
                        delegate: Tile {
                            required property var modelData
                            objectName: "appLibraryPicture"
                            url: modelData.url
                            name: modelData.name
                            enabled: pop.imageKey !== ""
                            onTapped: { pop.appView.useImage(pop.imageKey, modelData.path); pop.close() }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: pop.takeTiles.length === 0 && pop.pictureTiles.length === 0
                    text: pop.query !== "" ? qsTr("Nothing matches “%1”").arg(pop.query) : qsTr("Nothing yet. Runs and the pictures you use appear here.")
                    color: pop.look.label
                    font.pixelSize: pop.look.textSize
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    component Tile: Rectangle {
        id: tile
        property url url
        property string name
        property bool chosen: false
        signal tapped
        width: 26 * pop.u
        height: 26 * pop.u
        radius: pop.look.params.cornerWell * pop.look.unit
        color: pop.look.well
        border.width: chosen || activeFocus ? 2 : 1
        border.color: chosen || activeFocus ? pop.look.accent : pop.look.hairline
        opacity: enabled ? 1 : 0.5
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: name
        Keys.onReturnPressed: tapped()
        Keys.onSpacePressed: tapped()
        Image {
            anchors { fill: parent; margins: 2; bottomMargin: caption.height + 2 }
            source: tile.url
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize.height: 160
        }
        Text {
            id: caption
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 2 }
            text: tile.name
            elide: Text.ElideRight
            color: pop.look.label
            font.pixelSize: pop.look.captionSize
        }
        TapHandler { onTapped: tile.tapped() }
    }
}
