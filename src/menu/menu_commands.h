#pragma once

// The commands the menus add to the registry (issue #50) and the menu state of the canvas's
// commands: when each can run and whether a toggle is on.

#include <QString>
#include <QUrl>
#include <functional>

class CanvasModel;

namespace fugue {

class CommandRegistry;

// Registers the commands the menus offer beyond the 12.4 table, as not available until their
// owner sets a handler: file.open-clipboard (workflows set it), file.export-share and file.export-compiled (spec 11.5), edit.clear,
// one node.color.<role> per menus::colorRoles() (spec 4.5), help.docs, help.report-issue,
// help.about, app.menu-bar (F10) and app.context-menu (Shift+F10, Menu). Also gives the 12.4
// commands their shorter menu names. Needs registerDefaultCommands first; a command already
// registered is left as it is.
void registerMenuCommands(CommandRegistry &registry);

// The handlers that need the canvas (edit.clear, the colors) and the help links, and when
// the canvas's commands can run: undo with something to undo, the node commands with a
// selection, paste with something on the clipboard. `openUrl` opens a web page (tests pass
// their own).
void bindMenuCommands(CommandRegistry &registry, CanvasModel &canvas,
                      std::function<void(const QUrl &)> openUrl = {});

// Gives the Frame `frame` when set, else the selected nodes, the layout color `role` (a theme
// role; "none" takes the color off), as one undo step.
void applyLayoutColor(CanvasModel &canvas, const QString &role, const QString &frame = {});

} // namespace fugue
