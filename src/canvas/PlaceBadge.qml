import QtQuick
import QtQuick.Controls
import Fugue

// Where a model is available from (#164, docs/canvas-look.md 16.3): one small glyph, `monitor`
// for this machine and `server` for a paired host, with a tooltip that says it in words (the
// glyph is never the only signal). Informational: it takes no press, so it has no 28px floor;
// it is 16px, and dimmed with "offline" in its tip for a host that does not answer. The
// Templates cards and the model pickers' rows both draw it, so they say it the same way.
Item {
    id: badge
    required property CanvasLook look // App.theme.look
    // "local" (this machine) or "host" (a paired host).
    property string kind: "local"
    // The paired hosts' names, for "host".
    property var hosts: []
    // The host does not answer: its models stay listed, dimmed.
    property bool offline: false

    readonly property string tip: kind === "local" ? qsTr("On this machine")
                                  : offline ? qsTr("Host offline: %1").arg(hosts.join(", "))
                                  : qsTr("Shared by %1").arg(hosts.join(", "))
    implicitWidth: 16
    implicitHeight: 16
    opacity: offline ? 0.55 : 1
    Accessible.role: Accessible.StaticText
    Accessible.name: tip

    Glyph {
        anchors.centerIn: parent
        look: badge.look
        name: badge.kind === "local" ? "monitor" : "server"
        color: badge.look.label
        font.pixelSize: 16
    }
    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered
    ToolTip.delay: badge.look.tooltipDelay
    ToolTip.text: badge.tip
}
