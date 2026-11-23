#pragma once

// The window's menus as data (issue #50, spec 12.5): the menu bar and the canvas, node and
// Frame context menus (and the colors one the selection toolbar opens, #74), each a tree of
// command ids. Names, keys and enabled state come from
// the command registry (CommandRegistry::menu() merges them in), so a menu, the `?` overlay
// and the keymap never disagree, and a command registered later shows up where it is listed.
//
// An item is a map: {command: id} (with {swatch: role} on a color, and {hideWhenOff: true} when
// a context menu leaves it out where it does not apply, #72), {separator: true}, or a submenu
// {title, items} whose title is English with `&` before its mnemonic, translated in the "Menu"
// context.

#include <QString>
#include <QStringList>
#include <QVariantList>

namespace fugue::menus {

// bar, canvas, node, frame, tab (a workflow tab's, #59), colors (the selection toolbar's color button, #74),
// and run (the run toolbar's split button, #60)
QStringList names();
QVariantList tree(const QString &name);

// The layout colors offered (spec 4.5: theme roles, so they follow the palette), in menu
// order: none first, then accent, muted and the palette's hues. Each is a command
// node.color.<role>.
QStringList colorRoles();

// The menus of a view type's area header (#262, Blender's editor headers): the registry's tree name (`tree`: one of
// names()), the start of the object names its bar gets (`prefix`: two bars of one window never share a name), and
// whether the bar needs the view itself (`needsView`: the Canvas view says what applies and what is checked; the Graph's
// rows carry their own state). An empty tree: the view type has no menus in its header (an unknown type, a placeholder).
struct ViewMenus {
    QString tree;
    QString prefix;
    bool needsView = false;
};
ViewMenus viewMenus(const QString &viewType);

// The workflow tabs in the window bar (#262, Blender's workspace tabs in its top bar). `room` is the width the bar leaves
// the strip (the New button not counted). The tabs take their `preferred` width while they fit, then share the room
// down to `minimum`; past that they keep `minimum` and the strip scrolls (tabScroll). No tabs: 0.
qreal tabWidth(int count, qreal room, qreal preferred, qreal minimum);
// The scroll offset that keeps tab `current` whole in the strip, moving the strip `offset` the least it can; the offset
// stays inside the strip's content.
qreal tabScroll(int current, int count, qreal width, qreal room, qreal offset);

} // namespace fugue::menus
