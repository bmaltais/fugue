.pragma library

// The tone of the nearest KitPanel above an item (docs/canvas-look.md 20.4): the panel says `kitTone`, the toggles, choosers and
// sliders inside read it, so no row passes it down. "" when no panel is above (the control then uses the accent). Called from a
// binding, so a change of the panel's tone, or a move of the item, shows live.
function nearest(item) {
    for (let p = item ? item.parent : null; p; p = p.parent)
        if (p.kitTone !== undefined && p.kitTone !== "")
            return p.kitTone
    return ""
}
