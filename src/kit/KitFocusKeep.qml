pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// Put inside a Popup that fades out (#503). When a popup's exit ends, Qt gives the keyboard back to what had it before the popup opened,
// unconditionally. A dialog opened during the fade (Escape, then F1 within 150 ms) has the keyboard by then, and would lose it: its
// own Escape would do nothing. This remembers where the focus sat in any other popup while this one was going, and puts it back
// when this one is closed, if the keyboard is no longer there.
QtObject {
    id: keep

    required property Popup popup
    property Item overlay: null // where the popups sit, read while this one is shown (its item leaves the overlay when it is closed)
    property Item taken: null // the last item with the focus inside another popup, while this one was shown

    // True when another popup has the keyboard now (read in the popup's own onClosed, to leave it there).
    readonly property bool elsewhere: taken !== null && taken.visible

    // The popup item (a child of the overlay, as ours is) that holds `item`, or null when it is in no popup.
    function holder(item: Item): Item {
        if (!keep.overlay)
            return null
        for (let at = item; at; at = at.parent)
            if (at.parent === keep.overlay)
                return at
        return null
    }

    property Connections follow: Connections {
        target: keep.popup.parent ? keep.popup.parent.Window.window : null
        function onActiveFocusItemChanged() {
            const item = (target as Window).activeFocusItem
            const held = item ? keep.holder(item) : null
            if (held && held !== keep.popup.contentItem.parent)
                keep.taken = item
        }
    }

    property Connections ends: Connections {
        target: keep.popup
        function onAboutToShow() { keep.taken = null }
        function onOpened() { keep.overlay = keep.popup.contentItem.parent.parent }
        function onClosed() {
            const item = keep.taken // kept till the next show: the popup's own onClosed can ask `elsewhere`
            const window = keep.popup.parent ? keep.popup.parent.Window.window : null
            if (item && window && item.visible && window.activeFocusItem !== item && keep.holder(item))
                item.forceActiveFocus()
        }
    }
}
