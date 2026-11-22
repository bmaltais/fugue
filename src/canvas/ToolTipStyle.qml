import QtQuick
import QtQuick.Controls
import Fugue

// The one look of every tooltip in the window (docs/ui-ergonomics.md, Tooltips). The attached
// `ToolTip.text` and `ToolTip.visible` of every control share one popup of the Material style,
// which makes a tooltip as wide as its text is long (a node's whole description was one
// 1500 px line). This item, made once by Main.qml, bounds that popup: wrapped at `maxUnits`
// theme units (80u, 320 px at the default text size: about 45 characters), never wider than the window less its margins, shown below the
// control it names with its left edge on the control's, and only after `look.tooltipDelay` of resting there (every site sets `ToolTip.delay` to it: the
// attached `ToolTip.delay` is the control's own, the shared popup does not know it).
Item {
    id: style
    required property CanvasLook look // App.theme.look
    // The window's width: the tooltip never exceeds it.
    required property real windowWidth

    readonly property int maxUnits: 80
    readonly property real maxWidth: Math.min(maxUnits * look.unit, windowWidth - 2 * tip.margins)
    // The gap between the control and its tooltip.
    readonly property real gap: look.unit

    readonly property var tip: ToolTip.toolTip
    visible: false

    // No fade out: a tooltip fading for half a second under a pointer that keeps scrolling took
    // the next wheel events (it lay where the scrolled row had been). It goes at once.
    Component.onCompleted: tip.exit = null

    Binding {
        target: style.tip
        property: "implicitWidth"
        value: Math.min(style.tip.implicitContentWidth + style.tip.leftPadding + style.tip.rightPadding, style.maxWidth)
    }
    Binding { target: style.tip; property: "x"; value: 0 }
    Binding {
        target: style.tip
        property: "y"
        value: style.tip.parent ? style.tip.parent.height + style.gap : 0
    }
}
