#pragma once

// The commands of the bars and panels (#129), one `view.toggle-...` per element of the
// visibility registry (chrome_visibility.h): the menu bar, the workflow tabs, the run toolbar, the
// selection toolbar and the breadcrumb, each checked in the Bars and panels menu while it is shown; the node
// library and the workflows panel keep their `panel.nodes` and `panel.workflows` (side_panels.h) and
// the queue panel its `panel.queue` (run_bar.h). Also `view.focus-mode` (H: hide all the
// chrome, and the same key brings it back as it was) and `view.reset-bars` (every bar back at
// its default, from any state). `app.menu-bar` (F10) slides an auto-hidden menu bar in (the window does that, #160) and opens it; the
// remembered mode does not change.

namespace fugue {

class ChromeVisibility;
class CommandRegistry;

// Registers the commands' specs, without handlers (registerMenuCommands calls it, so every menu
// item is a registered command); a command already registered is left as it is.
void addChromeCommands(CommandRegistry &commands);

// Registers the bars with `chrome` (run-toolbar, and the side panels, register themselves) and
// sets the commands' handlers and checks. Needs registerDefaultCommands and registerMenuCommands first.
void registerChromeCommands(CommandRegistry &commands, ChromeVisibility &chrome);

} // namespace fugue
