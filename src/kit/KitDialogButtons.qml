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
    // The keyboard's focus ring of an accent-filled or solid red answer: 2px of accent (of the error colour on the red) 3px outside
    // it, as GNOME's focus outline; one item for the row, made the first time and put inside the focused answer (not a child of the
    // layout). Other answers draw their own on their edge.
    readonly property var ringTarget: own.buttons.find(b => b.activeFocus && b["ring"] === true && b["ringOutside"] === true) ?? null
    onRingTargetChanged: {
        if (ringTarget && !own.ring)
            own.ring = ringComponent.createObject(ringTarget) as Item
        else if (own.ring)
            own.ring.parent = ringTarget
    }
    Component {
        id: ringComponent
        Rectangle {
            objectName: "dialogOuterRing"
            readonly property var target: row.ringTarget
            visible: target !== null
            x: -3
            y: -3
            width: (target ? target.width : 0) + 6
            height: (target ? target.height : 0) + 6
            radius: (target ? target.radius : 0) + 3
            color: "transparent"
            border.width: 2
            border.color: target && target.solidDanger === true ? App.theme.look.error : App.theme.look.accent
        }
    }

    QtObject {
        id: own
        property var buttons: []
        property var watched: [] // the buttons whose role change re-arranges the row
        property Item ring: null // the outer focus ring, once made
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
    function usable(button): bool { // a KitChip that is `dimmed` looks off and does nothing
        return button !== null && button.visible && button.enabled && button["dimmed"] !== true
    }

    // Puts the buttons in the order above. Runs when the row is made, when a child is added and when a button's role changes; a
    // change of visibility moves nothing (the order is by role; the gaps follow the visible buttons by binding). Only children with a
    // `dialogRole` (ViewButton, ThemedButton, KitChip) are buttons; anything else stays where it is, at the start.
    function arrange() {
        const helpers = [apart, dangerBefore, dangerAfter]
        const found = []
        for (let i = 0; i < children.length; ++i) {
            if (!helpers.includes(children[i]) && children[i]["dialogRole"] !== undefined)
                found.push(children[i])
        }
        own.watched = own.watched.filter(b => b !== null && b !== undefined) // a destroyed button reads as null
        for (const b of found) {
            b["dialogRow"] = true // the dialog button's one size, and the solid red for a destructive answer
            b.Layout.minimumWidth = Qt.binding(() => b.implicitWidth) // a narrow row never squeezes a label
            if (!own.watched.includes(b)) {
                own.watched = own.watched.concat([b])
                b["dialogRoleChanged"].connect(row.schedule)
            }
        }
        const sorted = found.map((b, i) => ({ b: b, i: i, r: row.rank[row.roleOf(b)] }))
                            .sort((x, y) => x.r - y.r || x.i - y.i).map(e => e.b)
        own.buttons = sorted
        const wanted = sorted.filter(b => row.roleOf(b) === "extra")
                             .concat([apart], sorted.filter(b => row.roleOf(b) === "cancel"), [dangerBefore],
                                     sorted.filter(b => row.roleOf(b) === "destructive"), [dangerAfter],
                                     sorted.filter(b => row.roleOf(b) === "accept"))
        const now = []
        for (let i = 0; i < children.length; ++i) {
            if (wanted.includes(children[i]))
                now.push(children[i])
        }
        let first = 0
        while (first < wanted.length && now[first] === wanted[first])
            ++first
        if (first === wanted.length)
            return
        // From the first one out of place on, each is taken out and put back in order (a layout and the Tab chain follow the
        // children's order); the ones before it stay. A button that had the focus gets it back.
        const focused = wanted.slice(first).find(item => item.activeFocus) ?? null
        own.arranging = true
        for (const item of wanted.slice(first)) {
            item.parent = null
            item.parent = row
        }
        own.arranging = false
        if (focused)
            focused.forceActiveFocus(Qt.TabFocusReason)
    }
    function schedule() {
        Qt.callLater(row.arrange)
    }
    Component.onCompleted: arrange()
    onChildrenChanged: if (!own.arranging) row.schedule()

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
