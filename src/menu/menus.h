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

#include <QStringList>
#include <QVariantList>

namespace fugue::menus {

// bar, canvas, node, frame, and colors (the selection toolbar's color button, #74)
QStringList names();
QVariantList tree(const QString &name);

// The layout colors offered (spec 4.5: theme roles, so they follow the palette), in menu
// order: none first, then accent, muted and the palette's hues. Each is a command
// node.color.<role>.
QStringList colorRoles();

} // namespace fugue::menus
