.pragma library

// Where a popup menu opens so that it stays wholly inside the window. One place for every
// menu: context menus, bar menus, submenus and the widget menus.

// Clear space kept between a popup and the window's edge, in pixels.
var margin = 4

// One axis. `pos` is where the popup's near edge wants to be, `flipped` where it goes when it
// does not fit there (the other side of the point), `size` its length, `limit` the window's.
// Taken as wanted when it fits, else flipped when that fits, else shifted to fit; a popup
// longer than the window starts at the margin (its height is capped by `maxExtent`).
function axis(pos, flipped, size, limit) {
    if (pos + size + margin <= limit)
        return Math.max(pos, margin)
    if (flipped >= margin && flipped + size + margin <= limit)
        return flipped
    return Math.max(margin, Math.min(pos, limit - size - margin))
}

// The longest a popup may be in a window `limit` long.
function maxExtent(limit) {
    return Math.max(0, limit - 2 * margin)
}

// The point, in `anchor`'s coordinates, for a popup of size (w, h) that wants its top left at
// (x, y) of `anchor`, inside `overlay` (the window's popup layer). `gapX` and `gapY` are the
// room the anchor itself takes beside that point: 0 for a pointer, the anchor's size for a
// menu opening under a title (flipped, it goes above the title, aligned to its far edge).
function fit(anchor, overlay, x, y, w, h, gapX, gapY) {
    const at = anchor.mapToItem(overlay, x, y)
    const left = axis(at.x, at.x + gapX - w, w, overlay.width)
    const top = axis(at.y, at.y - gapY - h, Math.min(h, maxExtent(overlay.height)), overlay.height)
    return overlay.mapToItem(anchor, left, top)
}
