pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Fugue

// A dialog's actions, one row at its foot (docs/ui-ergonomics.md 2026-11-27 "One row of actions"; docs/canvas-look.md 20.24): every
// question, sheet and form dialog puts its buttons in this row, never a row of its own. The buttons are its children, the window's
// one button (ThemedButton or ViewButton, D4) or a KitChip, each with a `dialogRole`:
//   - "extra": another way out or a side action ("Download the template's own files…", "Read the license"), at the far start,
//     apart from the rest;
//   - "cancel": Cancel, Keep, Back, Not now, Close: the answer that changes nothing;
//   - "destructive": the answer that loses something (Discard, Delete, Quit), in the danger tint, its own chunk (twice the gap before
//     it, and after it when an acting answer follows);
//   - "accept" (the default when no role is given): the answers that act, the surface's primary last.
// The row lays them out in GNOME's order whatever their order in the file: the extras at the start, a stretch, Cancel, the destructive
// answer, the acting answers, so the answer that acts sits at the end (the bottom right; right to left it mirrors with the content).
//
// Keys: Left and Right move the focus between the row's buttons as they are seen (right to left too), Tab and Shift+Tab walk them in
// reading order; Enter or Space presses the focused button, the focus ring is the button's own (for the keyboard only). Escape with the
// focus in the row presses the cancel answer. A Question also takes Enter where no button and no text field has the focus and presses
// `defaultButton`, and Escape anywhere in it presses the cancel answer (Question.qml); another host calls pressDefault() and
// pressCancel() itself.
//
// The default answer is `defaultButton` when set, else the visible `primary` (highlighted) one: the acting answer, or in a question
// about a loss the safe one (D8).
RowLayout {
    id: row
    // The button Enter presses when no button has the focus; null: the visible button that is `primary`.
    property Item defaultButton: null
    // The buttons in the order shown, from the start (left, or right right to left) to the end.
    readonly property var buttons: own.buttons
    // Escape in the row presses the cancel answer; off where Escape means more (Projects: Back is the cancel answer of a view, and
    // Escape closes the whole dialog).
    property bool cancelOnEscape: true
    Layout.fillWidth: true
    spacing: 2 * App.theme.look.unit

    // The stretch between the extras and the rest (at least the row's gap twice), and the gaps around a destructive answer: a
    // visible item of no width adds one more gap.
    Item { id: apart; Layout.fillWidth: true }
    Item { id: dangerBefore; visible: row.hasRole("cancel") && row.hasRole("destructive") }
    Item { id: dangerAfter; visible: row.hasRole("destructive") && row.hasRole("accept") }

    QtObject {
        id: own
        property var buttons: []
        property bool arranging: false
    }

    readonly property var rank: ({ extra: 0, cancel: 1, destructive: 2, accept: 3 })
    function roleOf(button): string { // untyped: a ViewButton or a KitChip, both with `dialogRole`
        const role = button["dialogRole"]
        return role === "extra" || role === "cancel" || role === "destructive" ? role : "accept"
    }
    function hasRole(role: string): bool {
        return own.buttons.some(b => b.visible && row.roleOf(b) === role)
    }
    function usable(button): bool {
        return button !== null && button.visible && button.enabled
    }

    // Puts the children in the order above. Runs when the row is made and when a button is added (a Repeater, a Loader).
    function arrange() {
        const helpers = [apart, dangerBefore, dangerAfter]
        const found = []
        for (let i = 0; i < children.length; ++i) {
            if (!helpers.includes(children[i]))
                found.push(children[i])
        }
        const sorted = found.map((b, i) => ({ b: b, i: i, r: row.rank[row.roleOf(b)] }))
                            .sort((x, y) => x.r - y.r || x.i - y.i).map(e => e.b)
        own.buttons = sorted
        const wanted = sorted.filter(b => row.roleOf(b) === "extra")
                             .concat([apart], sorted.filter(b => row.roleOf(b) === "cancel"), [dangerBefore],
                                     sorted.filter(b => row.roleOf(b) === "destructive"), [dangerAfter],
                                     sorted.filter(b => row.roleOf(b) === "accept"))
        if (wanted.every((item, i) => children[i] === item))
            return
        own.arranging = true
        for (const item of wanted) { // taken out and put back in order: a layout and the Tab chain follow the children's order
            item.parent = null
            item.parent = row
        }
        own.arranging = false
    }
    Component.onCompleted: arrange()
    onChildrenChanged: if (!own.arranging) Qt.callLater(row.arrange)

    function defaultAction() { // untyped: a ViewButton or a KitChip, both with `tapped`
        if (defaultButton)
            return defaultButton
        return own.buttons.find(b => b.visible && b["primary"] === true) ?? null
    }
    // Presses the default answer; false when there is none that can act (the key then goes on to the host).
    function pressDefault(): bool {
        const button = defaultAction()
        if (!usable(button))
            return false
        button["tapped"]()
        return true
    }
    // Presses the cancel answer; false when there is none that can act.
    function pressCancel(): bool {
        const button = own.buttons.find(b => row.usable(b) && row.roleOf(b) === "cancel") ?? null
        if (!button)
            return false
        button["tapped"]()
        return true
    }
    // Moves the focus to the next button on screen, to the right (`toRight`) or to the left; it stops at the row's ends.
    function step(toRight: bool): bool {
        const focused = own.buttons.find(b => b.activeFocus) ?? null
        if (!focused)
            return false
        const at = focused.x
        let best = null
        for (const b of own.buttons) {
            if (b === focused || !usable(b) || b["activeFocusOnTab"] === false)
                continue
            if (toRight ? b.x > at && (!best || b.x < best.x) : b.x < at && (!best || b.x > best.x))
                best = b
        }
        if (best)
            best.forceActiveFocus(Qt.TabFocusReason)
        return true
    }
    Keys.onLeftPressed: event => event.accepted = row.step(false)
    Keys.onRightPressed: event => event.accepted = row.step(true)
    Keys.onEscapePressed: event => event.accepted = row.cancelOnEscape && row.pressCancel()
}
